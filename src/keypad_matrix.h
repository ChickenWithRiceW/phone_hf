#ifndef KEYPAD_MATRIX_H
#define KEYPAD_MATRIX_H

#include <Arduino.h>

namespace KeypadMatrix {

void begin();

// Returns a key once per press, or '\0'. Call every loop(); it also ends beeps.
char getKeyPressed();

void playBeep(uint16_t freqHz, uint16_t durationMs);
void stopSound();

}
#endif