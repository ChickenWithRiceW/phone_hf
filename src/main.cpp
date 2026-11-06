#include "esp32_hf.hpp"
#include "keypad_matrix.h"
#include <Arduino.h>

// IO-devices
#define SPEAKER_LRC_WS 25
#define SPEAKER_DOUT 26
#define SPEAKER_BCLK 5
#define MIC_ADC_PIN 34

#define HOOK_PIN 13
#define HOOK_CLOSED_MEANS_ON_CRADLE true
#define HOOK_DEBOUNCE_MS 40

 // Special key defines
#define KEY_RESET 'R'
#define KEY_OK 'K'
#define KEY_REDIAL 'W'
#define KEY_SPECIAL 'S'
#define SPECIAL_TIMEOUT_MS 8000
#define MAX_DIGITS 20

EspHf& hf = EspHf::getInstance();

// Sounds: one beep each (frequency, duration).
static void beepOk() { KeypadMatrix::playBeep(1400, 60); }
static void beepError() { KeypadMatrix::playBeep(300, 200); }
static void beepBusy() { KeypadMatrix::playBeep(400, 600); }

// Set by Bluetooth callbacks, played from loop().
enum { EV_NONE, EV_CONNECTED, EV_DISCONNECTED };
static volatile int event = EV_NONE;

// For dis and connection.
static void chime(bool rising)
{
    KeypadMatrix::playBeep(rising ? 660 : 1100, 90);
    delay(120);
    KeypadMatrix::playBeep(rising ? 1100 : 660, 150);
}

static bool handsetLifted = false;
static char dialBuf[MAX_DIGITS + 1] = {0};
static size_t dialLen = 0;
static bool specialMode = false;
static uint32_t specialLastKey = 0;
static bool micMuted = false;
static bool audioOnEsp = true;

static void clearDial()
{
    dialLen = 0;
    dialBuf[0] = '\0';
}

static void dialBuffer()
{
    if (dialLen == 0 || hf.getState() != IDLE_CONNECTED) {
        beepError();
        return;
    }
    Serial.printf("[DIAL] Calling '%s' -> %d\n", dialBuf, hf.callNumber(dialBuf));
    clearDial();
}

static void handleDigit(char key)
{
    if (dialLen >= MAX_DIGITS) {
        beepError();
        return;
    }
    dialBuf[dialLen++] = key;
    dialBuf[dialLen] = '\0';
    Serial.printf("[DIAL] %s\n", dialBuf);
}

static void setSpecial(bool on)
{
    specialMode = on;
    specialLastKey = millis();
    Serial.printf("[MODE] Special mode %s\n", on ? "ON" : "OFF");
    KeypadMatrix::playBeep(on ? 1800 : 1500, 100);
}

static void handleSpecialKey(char key)
{
    specialLastKey = millis();
    switch (key) {
    case '2':
    case '8': {
        const int volume = constrain(hf.getVolume() + (key == '2' ? 1 : -1), 0, 15);
        hf.setVolume(volume);
        Serial.printf("[VOL] %d\n", volume);
        KeypadMatrix::playBeep(500 + volume * 90, 90); // pitch follows volume
        break;
    }
    case '0':
        micMuted = !micMuted;
        hf.setMicMute(micMuted);
        Serial.printf("[MIC] %s\n", micMuted ? "muted" : "unmuted");
        KeypadMatrix::playBeep(micMuted ? 400 : 600, 150);
        break;
    case '5':
        if (!hf.isConnected()) {
            beepError();
            break;
        }
        Serial.printf("[AUDIO] move to %s -> %d\n", audioOnEsp ? "phone" : "ESP",
            audioOnEsp ? hf.disconnectAudio() : hf.connectAudio());
        audioOnEsp = !audioOnEsp;
        beepOk();
        break;
    case '1':
        Serial.printf("[INFO] connected=%d phone='%s' battery=%d signal=%d network=%d volume=%d\n",
            hf.isConnected(), hf.getPeerName(), hf.getBatteryLevel(), hf.getSignalStrength(),
            hf.isNetworkAvailable(), hf.getVolume());
        beepOk();
        break;
    case KEY_SPECIAL:
    case KEY_RESET:
        setSpecial(false);
        break;
    default:
        beepError();
        break;
    }
}

static void handleKey(char key)
{
    if (specialMode) {
        handleSpecialKey(key);
        return;
    }

    const e_state state = hf.getState();

    if (key >= '0' && key <= '9') {
        handleDigit(key);
    } else if (key == '*') {
        if (dialLen == 0) handleDigit('+'); // leading '*' = '+'
    } else if (key == KEY_SPECIAL) {
        setSpecial(true);
    } else if (key == KEY_RESET) {
        if (state == INCOMING_CALL) {
            Serial.printf("[CALL] Reject -> %d\n", hf.rejectIncomingCall());
        } else {
            clearDial();
            Serial.println("[DIAL] Reset.");
        }
    } else if (key == KEY_OK) {
        if (state == INCOMING_CALL)
            Serial.printf("[CALL] Accept -> %d\n", hf.acceptIncomingCall());
        else if (handsetLifted)
            dialBuffer();
        else
            beepError(); // lift the handset to dial
    } else if (key == KEY_REDIAL) {
        if (handsetLifted && state == IDLE_CONNECTED)
            Serial.printf("[DIAL] Redial -> %d\n", hf.redial());
        else
            beepError();
    }
}

static void onHandsetLifted()
{
    Serial.println("[HOOK] Handset lifted");
    switch (hf.getState()) {
    case INCOMING_CALL:
        hf.acceptIncomingCall();
        break;
    case IN_CALL:
        hf.connectAudio(); // take over a call already running on the phone
        audioOnEsp = true;
        break;
    case IDLE_CONNECTED:
        if (dialLen > 0)
            dialBuffer(); // number typed while the handset was down
        else
            beepOk();
        break;
    case IDLE_DISCONNECTED:
        beepError();
        break;
    default:
        break;
    }
}

static void onHandsetReplaced()
{
    Serial.println("[HOOK] Handset on cradle");
    const e_state state = hf.getState();
    if (state == IN_CALL || state == OUTGOING_CALL) hf.endCall();
    clearDial();
}

// Debounced hook switch; calls the handlers on a change.
static void pollHook()
{
    static bool lastRaw = false;
    static uint32_t changedAt = 0;

    const bool onCradle = (digitalRead(HOOK_PIN) == LOW) == HOOK_CLOSED_MEANS_ON_CRADLE;
    const bool raw = !onCradle;
    const uint32_t now = millis();

    if (raw != lastRaw) {
        lastRaw = raw;
        changedAt = now;
    }
    if (raw != handsetLifted && now - changedAt >= HOOK_DEBOUNCE_MS) {
        handsetLifted = raw;
        if (raw)
            onHandsetLifted();
        else
            onHandsetReplaced();
    }
}

void setup()
{
    Serial.begin(115200);

    KeypadMatrix::begin();
    pinMode(HOOK_PIN, INPUT_PULLUP);
    handsetLifted = (digitalRead(HOOK_PIN) == LOW) != HOOK_CLOSED_MEANS_ON_CRADLE;

    hf.setDeviceName("ESP-HF");
    hf.setLegacyPin("0000");
    hf.setupSpeaker(SPEAKER_BCLK, SPEAKER_LRC_WS, SPEAKER_DOUT);
    hf.setMicrophoneAdcPin(MIC_ADC_PIN);

    hf.setCbBtConnected([] { Serial.println("[BT] Connected"); event = EV_CONNECTED; });
    hf.setCbBtDisconnected([] { Serial.println("[BT] Disconnected"); event = EV_DISCONNECTED; });

    Serial.printf("begin() returned %d\n", hf.begin());
}

void loop()
{
    static e_state lastState = (e_state)-1;
    static uint32_t lastRing = 0;
    static bool ringing = false;

    // Bluetooth events.
    const int ev = event;
    event = EV_NONE;
    if (ev == EV_CONNECTED) chime(true);
    if (ev == EV_DISCONNECTED) chime(false);

    // Call state changes.
    const e_state state = hf.getState();
    if (state != lastState) {
        Serial.printf("[STATE] %d\n", state);
        const bool wasInCall = lastState == IN_CALL || lastState == OUTGOING_CALL;
        const bool nowIdle = state == IDLE_CONNECTED || state == IDLE_DISCONNECTED;
        if (wasInCall && nowIdle && handsetLifted) beepBusy(); // call ended, handset still up
        if (state == INCOMING_CALL && handsetLifted) KeypadMatrix::playBeep(1200, 150); // call waiting
        lastState = state;
    }

    // Ring only while the handset is down: one beep every 2 s.
    const bool shouldRing = state == INCOMING_CALL && !handsetLifted;
    if (shouldRing && millis() - lastRing >= 2000) {
        lastRing = millis();
        KeypadMatrix::playBeep(1000, 400);
    }
    if (ringing && !shouldRing) KeypadMatrix::stopSound();
    ringing = shouldRing;

    pollHook();

    const char key = KeypadMatrix::getKeyPressed(); // also ends beeps
    if (key != '\0') handleKey(key);

    if (specialMode && millis() - specialLastKey > SPECIAL_TIMEOUT_MS) setSpecial(false);

    delay(2);
}
