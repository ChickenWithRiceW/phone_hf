#include "keypad_matrix.h"

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 4

#define KEYPAD_BUZZER_PIN 33

#define KEYPAD_DEBOUNCE_MS 25
#define KEYPAD_SETTLE_US 50

// Key N (0-15, row by row) plays KEY_BASE_HZ + N * KEY_STEP_HZ.
#define KEY_BASE_HZ 500
#define KEY_STEP_HZ 120
#define KEY_TONE_MS 80

namespace {

const uint8_t rowPins[KEYPAD_ROWS] = { 23, 22, 21, 19 };
const uint8_t colPins[KEYPAD_COLS] = { 4, 16, 17, 18 };

const char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
    { '1', '2', '3', 'K' },
    { '4', '5', '6', 'S' },
    { '7', '8', '9', 'W' },
    { '*', '0', '#', 'R' },
};

uint32_t soundOffAt = 0; // 0 = no sound running

// Returns the pressed key's position, or false if none.
bool scan(uint8_t& foundRow, uint8_t& foundCol)
{
    for (uint8_t row = 0; row < KEYPAD_ROWS; ++row) {
        digitalWrite(rowPins[row], LOW);
        delayMicroseconds(KEYPAD_SETTLE_US);

        for (uint8_t col = 0; col < KEYPAD_COLS; ++col) {
            if (digitalRead(colPins[col]) == LOW) {
                digitalWrite(rowPins[row], HIGH);
                foundRow = row;
                foundCol = col;
                return true;
            }
        }
        digitalWrite(rowPins[row], HIGH);
    }
    return false;
}

}

namespace KeypadMatrix {

void begin()
{
    for (uint8_t col = 0; col < KEYPAD_COLS; ++col)
        pinMode(colPins[col], INPUT_PULLUP);
    for (uint8_t row = 0; row < KEYPAD_ROWS; ++row) {
        pinMode(rowPins[row], OUTPUT);
        digitalWrite(rowPins[row], HIGH);
    }

    ledcAttach(KEYPAD_BUZZER_PIN, 2000, 10);
    ledcWriteTone(KEYPAD_BUZZER_PIN, 0);
}

void playBeep(uint16_t freqHz, uint16_t durationMs)
{
    soundOffAt = millis() + durationMs;
    ledcWriteTone(KEYPAD_BUZZER_PIN, freqHz);
}

void stopSound()
{
    soundOffAt = 0;
    ledcWriteTone(KEYPAD_BUZZER_PIN, 0);
}

char getKeyPressed()
{
    static char lastRaw = '\0';
    static char reported = '\0';
    static uint32_t changedAt = 0;

    if (soundOffAt && (int32_t)(millis() - soundOffAt) >= 0)
        stopSound();

    uint8_t row = 0;
    uint8_t col = 0;
    const char raw = scan(row, col) ? keys[row][col] : '\0';
    const uint32_t now = millis();

    if (raw != lastRaw) {
        lastRaw = raw;
        changedAt = now;
    } else if (now - changedAt >= KEYPAD_DEBOUNCE_MS && raw != reported) {
        reported = raw;
        if (raw != '\0') {
            playBeep(KEY_BASE_HZ + KEY_STEP_HZ * (row * KEYPAD_COLS + col), KEY_TONE_MS);
            return raw;
        }
    }
    return '\0';
}

}
