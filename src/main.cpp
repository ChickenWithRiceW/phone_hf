#include "esp32_hf.hpp"
#include "keypad_matrix.h"
#include <Arduino.h>

// Speaker (I2S amplifier).
#define SPEAKER_LRC_WS 25
#define SPEAKER_DOUT 26
#define SPEAKER_BCLK 5

// Analog microphone signal into ESP32 ADC1 (GPIO 34 is input-only).
#define MIC_ADC_PIN 34

// Hook switch (the lever under the handset). Wired GPIO -> switch -> GND.
// Switch CLOSED = handset on the cradle. Flip the value if yours is inverted.
#define HOOK_PIN 13
#define HOOK_CLOSED_MEANS_ON_CRADLE true
#define HOOK_DEBOUNCE_MS 40


#define KEY_RESET 'R'
#define KEY_OK 'K'
#define KEY_REDIAL 'W'
#define KEY_SPECIAL 'S'
#define SPECIAL_TIMEOUT_MS 8000
#define MAX_DIGITS 20

struct Note {
    uint16_t hz;
    uint16_t ms;
};

static const Note SND_BT_CONNECTED[] = { { 660, 90 }, { 0, 30 }, { 880, 90 }, { 0, 30 }, { 1100, 150 } };
static const Note SND_BT_DISCONNECTED[] = { { 1100, 90 }, { 0, 30 }, { 880, 90 }, { 0, 30 }, { 660, 150 } };
static const Note SND_PAIR_OK[] = { { 880, 80 }, { 0, 30 }, { 1320, 180 } };
static const Note SND_PAIR_FAIL[] = { { 300, 350 } };

// Looping ring: two warbles, then a pause.
static const Note SND_RING[] = { { 1000, 70 }, { 0, 40 }, { 800, 70 }, { 0, 40 }, { 1000, 70 }, { 0, 40 }, { 800, 70 }, { 0, 1700 } };

// Short double beep when a call comes in while the handset is already lifted.
static const Note SND_CALL_WAITING[] = { { 1200, 80 }, { 0, 80 }, { 1200, 80 } };

// Remote side hung up / call failed while the handset is still lifted.
static const Note SND_BUSY[] = { { 400, 220 }, { 0, 220 }, { 400, 220 }, { 0, 220 }, { 400, 220 }, { 0, 220 } };
static const Note SND_ERROR[] = { { 300, 150 }, { 0, 60 }, { 300, 150 } };
static const Note SND_OK[] = { { 1400, 60 } };
static const Note SND_RESET[] = { { 700, 60 }, { 0, 30 }, { 500, 90 } };
static const Note SND_SPECIAL_ON[] = { { 1500, 60 }, { 0, 40 }, { 1800, 80 } };
static const Note SND_SPECIAL_OFF[] = { { 1800, 60 }, { 0, 40 }, { 1500, 80 } };
static const Note SND_MUTE_ON[] = { { 500, 80 }, { 0, 30 }, { 350, 120 } };
static const Note SND_MUTE_OFF[] = { { 350, 80 }, { 0, 30 }, { 500, 120 } };

// Tiny non-blocking melody player on top of the keypad's buzzer.
namespace Player {
const Note* notes = nullptr;
size_t count = 0;
size_t index = 0;
bool looping = false;
uint32_t stepEnd = 0;

void stop()
{
    notes = nullptr;
    KeypadMatrix::stopSound();
}

void start(const Note* seq, size_t n, bool loop = false)
{
    notes = seq;
    count = n;
    index = 0;
    looping = loop;
    stepEnd = millis(); // first step starts on the next update()
}

void update()
{
    if (!notes) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - stepEnd) < 0) return;

    if (index >= count) {
        if (looping) {
            index = 0;
        } else {
            notes = nullptr;
            return;
        }
    }
    const Note& n = notes[index++];
    if (n.hz == 0)
        KeypadMatrix::stopSound();
    else
        KeypadMatrix::playBeep(n.hz, n.ms);
    stepEnd = now + n.ms;
}
} // namespace Player

template <size_t N>
static void play(const Note (&seq)[N], bool loop = false)
{
    Player::start(seq, N, loop);
}


static volatile bool evConnected = false;
static volatile bool evDisconnected = false;
static volatile bool evPairOk = false;
static volatile bool evPairFail = false;

EspHf& hf = EspHf::getInstance();

void cbIncoming(char phone_number[15], bool sim_call)
{
    if (sim_call)
        Serial.printf("[CALL] Incoming from SIM. Number '%s'\n", phone_number);
    else
        Serial.println("[CALL] Incoming from some app (no number).");
}
void cbOutgoing(char phone_number[15], bool sim_call)
{
    if (sim_call)
        Serial.printf("[CALL] Outgoing from SIM. Number '%s'\n", phone_number);
    else
        Serial.println("[CALL] Outgoing from some app (no number).");
}
void cbConnected(void)
{
    Serial.println("[BT] Connected");
    evConnected = true;
}
void cbDisconnected(void)
{
    Serial.println("[BT] Disconnected");
    evDisconnected = true;
}
void cbPairOk(void)
{
    Serial.println("[BT] Pairing worked");
    evPairOk = true;
}
void cbPairFail(void)
{
    Serial.println("[BT] Pairing failed");
    evPairFail = true;
}
void cbSspAuth(char* code) { Serial.printf("[BT] SSP code: %s\n", code); }
void cbLegacyAuth(void) { Serial.println("[BT] Legacy PIN requested"); }
void cbCallActive(void) { Serial.println("[CALL] Active"); }
void cbCallEnded(void) { Serial.println("[CALL] Ended"); }
void cbAudioOn(void) { Serial.println("[AUDIO] Connected"); }
void cbAudioOff(void) { Serial.println("[AUDIO] Disconnected"); }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const char* stateName(e_state state)
{
    switch (state) {
    case IDLE_DISCONNECTED: return "IDLE_DISCONNECTED";
    case IDLE_CONNECTED: return "IDLE_CONNECTED";
    case IN_CALL: return "IN_CALL";
    case INCOMING_CALL: return "INCOMING_CALL";
    case OUTGOING_CALL: return "OUTGOING_CALL";
    }
    return "UNKNOWN";
}

static void printInfo()
{
    Serial.printf("[INFO] connected=%d phone='%s' battery=%d signal=%d network=%d volume=%d\n",
        hf.isConnected(), hf.getPeerName(), hf.getBatteryLevel(), hf.getSignalStrength(),
        hf.isNetworkAvailable(), hf.getVolume());
}

// ---------------------------------------------------------------------------
// Hook switch (debounced). Returns true while the handset is lifted.
// ---------------------------------------------------------------------------
static bool handsetLifted = false;

static bool readHookRaw()
{
    const bool closed = digitalRead(HOOK_PIN) == LOW; // pulled up, switch to GND
    const bool onCradle = (closed == HOOK_CLOSED_MEANS_ON_CRADLE);
    return !onCradle;
}

// Returns +1 on a fresh lift, -1 on a fresh hang-up, 0 otherwise.
static int pollHook()
{
    static bool lastRaw = false;
    static uint32_t changedAt = 0;

    const bool raw = readHookRaw();
    const uint32_t now = millis();
    if (raw != lastRaw) {
        lastRaw = raw;
        changedAt = now;
    }
    if (raw != handsetLifted && now - changedAt >= HOOK_DEBOUNCE_MS) {
        handsetLifted = raw;
        return raw ? +1 : -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Dialing and keys
// ---------------------------------------------------------------------------
static char dialBuf[MAX_DIGITS + 1];
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
    if (dialLen == 0) {
        Serial.println("[DIAL] Nothing to dial.");
        play(SND_ERROR);
        return;
    }
    if (hf.getState() != IDLE_CONNECTED) {
        Serial.printf("[DIAL] Cannot dial in state %s\n", stateName(hf.getState()));
        play(SND_ERROR);
        return;
    }
    Serial.printf("[DIAL] Calling '%s' -> %d\n", dialBuf, hf.callNumber(dialBuf));
    clearDial();
}

static void handleDigit(char key)
{
    if (dialLen >= MAX_DIGITS) {
        play(SND_ERROR);
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
    if (on)
        play(SND_SPECIAL_ON);
    else
        play(SND_SPECIAL_OFF);
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
        // Confirmation beep: pitch follows the volume, so you can hear the step.
        KeypadMatrix::playBeep(500 + volume * 90, 90);
        break;
    }
    case '0':
        micMuted = !micMuted;
        hf.setMicMute(micMuted);
        Serial.printf("[MIC] %s\n", micMuted ? "muted" : "unmuted");
        if (micMuted)
            play(SND_MUTE_ON);
        else
            play(SND_MUTE_OFF);
        break;
    case '5':
        if (!hf.isConnected()) {
            play(SND_ERROR);
            break;
        }
        Serial.printf("[AUDIO] move to %s -> %d\n", audioOnEsp ? "phone" : "ESP",
            audioOnEsp ? hf.disconnectAudio() : hf.connectAudio());
        audioOnEsp = !audioOnEsp;
        play(SND_OK);
        break;
    case '1':
        printInfo();
        play(SND_OK);
        break;
    case KEY_SPECIAL:
        setSpecial(false);
        break;
    case KEY_RESET:
        setSpecial(false);
        break;
    default:
        play(SND_ERROR); // key has no special function
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
        if (dialLen == 0) handleDigit('+'); // leading '*' = '+' for international numbers
    } else if (key == KEY_SPECIAL) {
        setSpecial(true);
    } else if (key == KEY_RESET) {
        if (state == INCOMING_CALL) {
            Serial.printf("[CALL] Reject -> %d\n", hf.rejectIncomingCall());
        } else {
            clearDial();
            Serial.println("[DIAL] Reset.");
            play(SND_RESET);
        }
    } else if (key == KEY_OK) {
        if (state == INCOMING_CALL) {
            Serial.printf("[CALL] Accept -> %d\n", hf.acceptIncomingCall());
        } else if (handsetLifted) {
            dialBuffer();
        } else {
            Serial.println("[DIAL] Lift the handset to dial.");
            play(SND_ERROR);
        }
    } else if (key == KEY_REDIAL) {
        if (handsetLifted && state == IDLE_CONNECTED) {
            Serial.printf("[DIAL] Redial -> %d\n", hf.redial());
        } else {
            play(SND_ERROR);
        }
    }
    // '#' is ignored.
}

static void onHandsetLifted()
{
    Serial.println("[HOOK] Handset lifted");
    const e_state state = hf.getState();
    switch (state) {
    case INCOMING_CALL:
        Serial.printf("[CALL] Accept -> %d\n", hf.acceptIncomingCall());
        break;
    case IN_CALL:
        hf.connectAudio(); // call already running on the phone: take it over
        audioOnEsp = true;
        break;
    case IDLE_CONNECTED:
        if (dialLen > 0)
            dialBuffer(); // a number was typed while the handset was down
        else
            play(SND_OK); // ready for digits
        break;
    case OUTGOING_CALL:
        break;
    case IDLE_DISCONNECTED:
        Serial.println("[HOOK] No phone connected.");
        play(SND_ERROR);
        break;
    }
}

static void onHandsetReplaced()
{
    Serial.println("[HOOK] Handset on cradle");
    const e_state state = hf.getState();
    if (state == IN_CALL || state == OUTGOING_CALL) {
        Serial.printf("[CALL] Hang up -> %d\n", hf.endCall());
    }
    clearDial();
}

// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    Serial.println("Begun.");

    KeypadMatrix::begin();
    pinMode(HOOK_PIN, INPUT_PULLUP);
    handsetLifted = readHookRaw(); // no edge event for the state at boot
    clearDial();

    // Mandatory setup.
    hf.setDeviceName("ESP-HF");
    hf.setLegacyPin("0000");
    hf.setupSpeaker(SPEAKER_BCLK, SPEAKER_LRC_WS, SPEAKER_DOUT);
    hf.setMicrophoneAdcPin(MIC_ADC_PIN);

    // Optional callbacks.
    hf.setCbIncomingCall(cbIncoming);
    hf.setCbOutgoingCall(cbOutgoing);
    hf.setCbBtConnected(cbConnected);
    hf.setCbBtDisconnected(cbDisconnected);
    hf.setCbPinCorrect(cbPairOk);
    hf.setCbPinIncorrect(cbPairFail);
    hf.setCbSspAuth(cbSspAuth);
    hf.setCbLegacyAuth(cbLegacyAuth);
    hf.setCbCallActive(cbCallActive);
    hf.setCbCallEnded(cbCallEnded);
    hf.setCbAudioConnected(cbAudioOn);
    hf.setCbAudioDisconnected(cbAudioOff);

    Serial.printf("begin() returned %d\n", hf.begin());
    Serial.printf("[HOOK] Handset is %s\n", handsetLifted ? "LIFTED" : "on the cradle");
}

void loop()
{
    static e_state lastState = (e_state)-1;

    // --- Bluetooth events (set by callbacks) ---------------------------------
    if (evConnected) { evConnected = false; play(SND_BT_CONNECTED); }
    if (evDisconnected) { evDisconnected = false; play(SND_BT_DISCONNECTED); }
    if (evPairOk) { evPairOk = false; play(SND_PAIR_OK); }
    if (evPairFail) { evPairFail = false; play(SND_PAIR_FAIL); }

    // --- Call state changes ---------------------------------------------------
    const e_state state = hf.getState();
    if (state != lastState) {
        Serial.printf("[STATE] %s (%d)\n", stateName(state), state);

        // Call ended or failed while the handset is still lifted -> busy tone.
        if ((lastState == IN_CALL || lastState == OUTGOING_CALL) && (state == IDLE_CONNECTED || state == IDLE_DISCONNECTED) && handsetLifted) {
            play(SND_BUSY);
        }
        // Call arrives while the handset is already lifted -> call-waiting beeps.
        if (state == INCOMING_CALL && handsetLifted) {
            play(SND_CALL_WAITING);
        }
        lastState = state;
    }

    // --- Ringing: only while the handset is down -----------------------------
    const bool shouldRing = (state == INCOMING_CALL) && !handsetLifted;
    if (shouldRing && Player::notes == nullptr) {
        play(SND_RING, true);
    } else if (!shouldRing && Player::looping && Player::notes == SND_RING) {
        Player::stop();
    }

    // --- Hook switch ----------------------------------------------------------
    const int hook = pollHook();
    if (hook > 0) onHandsetLifted();
    if (hook < 0) onHandsetReplaced();

    // --- Keypad ---------------------------------------------------------------
    const char key = KeypadMatrix::getKeyPressed(); // also keeps the buzzer running
    if (key != '\0') handleKey(key);

    if (specialMode && millis() - specialLastKey > SPECIAL_TIMEOUT_MS) setSpecial(false);

    Player::update();
    delay(2);
}
