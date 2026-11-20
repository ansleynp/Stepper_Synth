/*
 * ESP32 MIDI + Serial Piano  ->  3 square-wave voices ("motors") on GPIO22/21/19
 * Framework: ESP-IDF (6.x)
 *
 * Voice manager:
 *   - Each new note goes to a free motor.
 *   - If all 3 are busy, the motor that has been playing the longest is
 *     taken over by the new note (oldest-note stealing).
 *   - Re-pressing a note that is already playing restarts it on the same motor.
 *   - Each motor has its own LEDC timer, so all 3 play independent pitches,
 *     each with its own vibrato.
 *
 * MIDI IN (5-pin DIN, through an optocoupler) on GPIO16, UART2 @ 31250.
 *   Note On -> start, Note Off (or vel 0) -> stop, CC 120/123 -> all off.
 *
 * USB serial monitor keyboard (115200):
 *
 *     w   e       t   y   u       o   p
 *   a   s   d   f   g   h   j   k   l   ;
 *   C   D   E   F   G   A   B   C   D   E
 *
 *   z / x : octave down / up      - / = : note length (0 = sustain)
 *   space : stop everything       v : vibrato on/off      ? : help
 *   n     : voice mode: poly -> mono (motor 1 only) -> unison (all motors, same note)
 *
 * Rotary encoder (A=GPIO12, B=GPIO14, button=GPIO13) settings menu:
 *   turn = choose a setting, click = edit it, turn = change value, click = done
 *
 * Transport push buttons (pin -> button -> GND, internal pull-ups; pins set in
 * TRANSPORT BUTTONS below). Serial monitor keys 1 / 2 / 3 press the same buttons:
 *   1 / D25 : start / stop recording (the take starts on its first note)
 *   2 / D26 : play / pause the recording (it loops)
 *   3 / D27 : reset - stop playback and rewind to the start
 * The take lives in SRAM and keeps looping with the computer unplugged, as
 * long as the board stays powered. The desktop app (desktop_app/) can save it
 * as a .mid file or load a .mid file onto the board using JSON lines on this
 * same serial port - see desktop_app/FIRMWARE_INTERFACE.md.
 */

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "hal/ledc_ll.h"   // direct access to the timer's fractional divider
#include "driver/uart.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

// =====================================================================
//                     VIBRATO SETTINGS  (tune these)
// =====================================================================
static bool     VIBRATO_ENABLED     = true;   // 'v' key toggles at runtime
static float    VIBRATO_RATE_HZ     = 5.5f;   // wobble speed (typical 4 - 7 Hz)
static float    VIBRATO_DEPTH_CENTS = 30.0f;  // +/- pitch swing; 100 cents = 1 semitone
static uint32_t VIBRATO_DELAY_MS    = 250;    // note plays straight this long before vibrato starts
static uint32_t VIBRATO_FADE_MS     = 400;    // then depth ramps from 0 to full over this long
static int      VIBRATO_SHAPE       = 0;      // 0 = sine, 1 = triangle
static constexpr uint32_t VIBRATO_UPDATE_US = 1000;  // pitch recompute period (1 ms)
// =====================================================================
//                     VOICE SETTINGS
// =====================================================================
static int      VOICE_MODE          = 0;      // 0 = poly   (up to 3 different notes)
                                              // 1 = mono   (1 note, motor 1 only)
                                              // 2 = unison (1 note on ALL motors - louder)
                                              // 'n' key or the knob cycles it
static float    UNISON_DETUNE_CENTS = 0.0f;   // unison only: spread motors apart for a thicker
                                              // sound (e.g. 5 = M1 -5c, M2 0c, M3 +5c)
static const char *const VOICE_MODE_NAMES[] = {"poly", "mono", "unison"};
// =====================================================================

// ---------------- configuration ----------------
// One output per motor. Add/remove pins here (max 4: one LEDC timer each).
static constexpr int        VOICE_PINS[]  = {22, 21, 19};   // M1, M2, M3 (mono plays on M1 = first pin)
static constexpr int        NUM_VOICES    = sizeof(VOICE_PINS) / sizeof(VOICE_PINS[0]);
static_assert(NUM_VOICES >= 1 && NUM_VOICES <= 4, "LEDC low-speed mode has 4 timers");
static constexpr int        MIDI_RX_PIN   = 16;          // MIDI data in
static constexpr int        MIDI_TX_PIN   = 17;          // test output for 'm' loopback test
static constexpr int        ENC_A_PIN     = 12;          // rotary encoder A (CLK)
static constexpr int        ENC_B_PIN     = 14;          // rotary encoder B (DT)
static constexpr int        ENC_BTN_PIN   = 13;           // encoder push button (SW), to GND when pressed
static constexpr int        ENC_STEPS_PER_DETENT = 4;    // most encoders: 4 (try 2 if one click = 2 steps)
static constexpr bool       ENC_REVERSE   = false;       // true if clockwise goes the wrong way
// =====================================================================
//                     TRANSPORT BUTTONS  (change pins here)
// =====================================================================
// Wire each button from its pin to GND; the internal pull-up is enabled.
static constexpr int        BTN_RECORD_PIN = 25;         // D25: start / stop recording
static constexpr int        BTN_PLAY_PIN   = 26;         // D26: play / pause (loops)
static constexpr int        BTN_RESET_PIN  = 27;         // D27: stop playback + rewind
static constexpr uint32_t   BTN_DEBOUNCE_MS = 30;        // level must be steady this long
// =====================================================================
static constexpr bool pinUsedByMotor(int p) {
  for (int v = 0; v < NUM_VOICES; v++) if (VOICE_PINS[v] == p) return true;
  return false;
}
static constexpr int OTHER_PINS[] = {MIDI_RX_PIN, MIDI_TX_PIN, ENC_A_PIN, ENC_B_PIN, ENC_BTN_PIN,
                                     BTN_RECORD_PIN, BTN_PLAY_PIN, BTN_RESET_PIN};
static constexpr bool pinsAreDistinct() {
  constexpr int n = sizeof(OTHER_PINS) / sizeof(OTHER_PINS[0]);
  for (int i = 0; i < n; i++) {
    if (pinUsedByMotor(OTHER_PINS[i])) return false;
    for (int j = i + 1; j < n; j++) if (OTHER_PINS[i] == OTHER_PINS[j]) return false;
  }
  return true;
}
static_assert(pinsAreDistinct(),
              "Two features (motor / MIDI / encoder / buttons) share a GPIO - pick different pins");
static constexpr uart_port_t MIDI_UART    = UART_NUM_2;
static constexpr uart_port_t CONSOLE_UART = UART_NUM_0;  // USB serial (CP2102)
static int                  MIDI_CHANNEL  = 0;           // 0 = all channels, 1-16 = only that one (knob-adjustable)
static constexpr bool       MIDI_DEBUG    = true;        // print every note event
static constexpr bool       RAW_DEBUG     = true;        // print every raw UART byte (hex) + UART errors
static constexpr bool       PIN_MONITOR   = false;       // every 2 s: print RX pin level + edge count

static constexpr ledc_mode_t      LEDC_MODE    = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_bit_t LEDC_RES     = LEDC_TIMER_10_BIT;
static constexpr uint32_t         DUTY_50      = 1u << (10 - 1);  // 512 of 1024

static constexpr int      MIN_OCTAVE   = 1;
static constexpr int      MAX_OCTAVE   = 7;
static constexpr uint32_t NOTE_STEP_MS = 50;
static constexpr uint32_t MAX_NOTE_MS  = 2000;

// ---------------- state (guarded by stateLock) ----------------
static SemaphoreHandle_t stateLock;

static int      octave    = 4;      // 'a' = C4 (middle C)
static uint32_t noteMs    = 300;    // 0 = sustain

static const char *NOTE_NAMES[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};

// ---------------- helpers ----------------
static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

static void con_printf(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) uart_write_bytes(CONSOLE_UART, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

static float midiToFreq(int midi) {
  return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

// ---------------- tone output (LEDC): one timer + channel per motor -------
static ledc_timer_t   voiceTimer(int v)   { return (ledc_timer_t)v; }
static ledc_channel_t voiceChannel(int v) { return (ledc_channel_t)v; }

static void toneInit() {
  for (int v = 0; v < NUM_VOICES; v++) {
    ledc_timer_config_t t;
    memset(&t, 0, sizeof(t));
    t.speed_mode      = LEDC_MODE;
    t.duty_resolution = LEDC_RES;
    t.timer_num       = voiceTimer(v);
    t.freq_hz         = 1000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c;
    memset(&c, 0, sizeof(c));
    c.gpio_num   = VOICE_PINS[v];
    c.speed_mode = LEDC_MODE;
    c.channel    = voiceChannel(v);
    c.timer_sel  = voiceTimer(v);
    c.duty       = 0;               // start silent
    c.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
  }
}

// ---------------- pitch engine (vibrato) ----------------
// The voice manager only sets each motor's *base* frequency. A 1 ms esp_timer
// callback is the only thing that touches the LEDC hardware: for every motor
// it adds that motor's vibrato and writes the timer's fractional divider
// directly, so pitch changes are smooth instead of rounding to whole Hz.
static portMUX_TYPE pitchMux = portMUX_INITIALIZER_UNLOCKED;
static float   engBaseFreq[NUM_VOICES];   // 0 = silent
static int64_t engStartUs[NUM_VOICES];    // when the current note started

static void engineSet(int v, float freqHz, int64_t startUs) {
  portENTER_CRITICAL(&pitchMux);
  engBaseFreq[v] = freqHz;
  engStartUs[v]  = startUs;
  portEXIT_CRITICAL(&pitchMux);
}

static float lfo(float phase01) {      // returns -1 .. +1
  if (VIBRATO_SHAPE == 1) {           // triangle
    float p = phase01 - floorf(phase01);
    return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
  }
  return sinf(2.0f * (float)M_PI * phase01);  // sine
}

static void pitchTimerCb(void *) {
  static bool     outputOn[NUM_VOICES]  = {};
  static int64_t  lastStart[NUM_VOICES] = {};
  static float    baseDiv[NUM_VOICES]   = {};   // exact 10.8 divider for the un-bent note
  static uint32_t lastDiv[NUM_VOICES]   = {};
  static bool     first = true;
  if (first) {
    for (int v = 0; v < NUM_VOICES; v++) lastStart[v] = -1;
    first = false;
  }

  int64_t now = esp_timer_get_time();

  for (int v = 0; v < NUM_VOICES; v++) {
    portENTER_CRITICAL(&pitchMux);
    float   base  = engBaseFreq[v];
    int64_t start = engStartUs[v];
    portEXIT_CRITICAL(&pitchMux);

    if (base <= 0.0f) {                         // silence
      if (outputOn[v]) {
        ledc_set_duty(LEDC_MODE, voiceChannel(v), 0);
        ledc_update_duty(LEDC_MODE, voiceChannel(v));
        outputOn[v] = false;
      }
      lastStart[v] = -1;
      continue;
    }

    // --- new note on this motor: let the driver pick the clock source
    //     (APB or REF_TICK) with a whole-Hz frequency, then correct the
    //     divider to the exact pitch.
    if (start != lastStart[v]) {
      lastStart[v] = start;
      uint32_t fInt = (uint32_t)lroundf(base);
      if (fInt < 1) fInt = 1;
      ledc_timer_config_t t;
      memset(&t, 0, sizeof(t));
      t.speed_mode      = LEDC_MODE;
      t.duty_resolution = LEDC_RES;
      t.timer_num       = voiceTimer(v);
      t.freq_hz         = fInt;
      t.clk_cfg         = LEDC_AUTO_CLK;
      if (ledc_timer_config(&t) != ESP_OK) continue;
      uint32_t d;
      ledc_ll_get_clock_divider(LEDC_LL_GET_HW(0), LEDC_MODE, voiceTimer(v), &d);
      baseDiv[v] = (float)d * (float)fInt / base;   // divider ~ 1 / freq
      lastDiv[v] = 0;                               // force a write below
    }

    // --- vibrato offset in cents (per motor, timed from its own note start) ---
    float cents = 0.0f;
    if (VOICE_MODE == 2 && NUM_VOICES > 1)   // unison detune: spread evenly from -d to +d
      cents += UNISON_DETUNE_CENTS * (2.0f * v / (NUM_VOICES - 1) - 1.0f);
    if (VIBRATO_ENABLED && VIBRATO_DEPTH_CENTS > 0.0f) {
      float sinceDelay = (now - start) / 1000.0f - (float)VIBRATO_DELAY_MS;
      if (sinceDelay > 0.0f) {
        float fade = VIBRATO_FADE_MS ? sinceDelay / (float)VIBRATO_FADE_MS : 1.0f;
        if (fade > 1.0f) fade = 1.0f;
        cents += VIBRATO_DEPTH_CENTS * fade * lfo(VIBRATO_RATE_HZ * sinceDelay / 1000.0f);
      }
    }
    // higher pitch = smaller divider (10.8 fixed point, valid range 1.0 .. 1023.99)
    float    divF = baseDiv[v] * powf(2.0f, -cents / 1200.0f);
    uint32_t div  = divF < 256.0f ? 256u : divF > 262143.0f ? 262143u : (uint32_t)lroundf(divF);

    if (div != lastDiv[v]) {
      ledc_ll_set_clock_divider(LEDC_LL_GET_HW(0), LEDC_MODE, voiceTimer(v), div);
      ledc_ll_ls_timer_update(LEDC_LL_GET_HW(0), LEDC_MODE, voiceTimer(v));  // latch it
      lastDiv[v] = div;
    }
    if (!outputOn[v]) {
      ledc_set_duty(LEDC_MODE, voiceChannel(v), DUTY_50);
      ledc_update_duty(LEDC_MODE, voiceChannel(v));
      outputOn[v] = true;
    }
  }
}

static void pitchEngineStart() {
  esp_timer_create_args_t args;
  memset(&args, 0, sizeof(args));
  args.callback = pitchTimerCb;
  args.name     = "pitch";
  esp_timer_handle_t h;
  ESP_ERROR_CHECK(esp_timer_create(&args, &h));
  ESP_ERROR_CHECK(esp_timer_start_periodic(h, VIBRATO_UPDATE_US));
}

// ---------------- voice manager (call with stateLock held) ----------------
struct Voice {
  bool     active;
  uint8_t  note;         // MIDI note number
  int64_t  startUs;      // when this note started (used to find the oldest)
  uint32_t releaseAtMs;  // 0 = hold until note off; else auto-stop time (keyboard)
};
static Voice voices[NUM_VOICES];

// Notes currently held down (sustained, oldest first). Mono/unison use this to
// fall back to the previous key when you let go of the newest one.
static constexpr int MAX_HELD = 16;
static uint8_t heldNotes[MAX_HELD];
static int     heldCount = 0;

static void heldRemove(uint8_t note) {
  for (int i = 0; i < heldCount; i++) {
    if (heldNotes[i] == note) {
      for (int j = i; j + 1 < heldCount; j++) heldNotes[j] = heldNotes[j + 1];
      heldCount--;
      return;
    }
  }
}

static void heldPush(uint8_t note) {
  heldRemove(note);
  if (heldCount == MAX_HELD) {                 // full: forget the oldest
    for (int j = 0; j + 1 < heldCount; j++) heldNotes[j] = heldNotes[j + 1];
    heldCount--;
  }
  heldNotes[heldCount++] = note;
}

static void voiceStop(int v) {
  voices[v].active = false;
  engineSet(v, 0.0f, 0);
}

static void printStart(const char *tag, uint8_t note, const char *where, bool replaced, uint8_t oldNote) {
  if (!MIDI_DEBUG) return;
  con_printf("%s %-2s%d (%3d) %8.2f Hz -> %s", tag, NOTE_NAMES[note % 12],
             note / 12 - 1, note, midiToFreq(note), where);
  if (replaced) con_printf("  (replaced %s%d)", NOTE_NAMES[oldNote % 12], oldNote / 12 - 1);
  con_printf("\r\n");
}

// Put a note on a specific motor (restarts its pitch + vibrato).
static void startVoice(int v, uint8_t note, uint32_t holdMs, const char *tag, bool print) {
  bool    replaced = voices[v].active && voices[v].note != note;
  uint8_t oldNote  = voices[v].note;
  int64_t now      = esp_timer_get_time();
  voices[v].active      = true;
  voices[v].note        = note;
  voices[v].startUs     = now;
  voices[v].releaseAtMs = holdMs ? millis() + holdMs : 0;
  engineSet(v, midiToFreq(note), now);
  if (print) {
    char where[16];
    snprintf(where, sizeof(where), "M%d", v + 1);
    printStart(tag, note, where, replaced, oldNote);
  }
}

// Mono: note on motor 1 only.  Unison: same note on every motor.
static void startGroup(uint8_t note, uint32_t holdMs, const char *tag) {
  if (VOICE_MODE == 2) {
    bool    replaced = voices[0].active && voices[0].note != note;
    uint8_t oldNote  = voices[0].note;
    for (int v = 0; v < NUM_VOICES; v++) startVoice(v, note, holdMs, tag, false);
    printStart(tag, note, "all motors", replaced, oldNote);
  } else {
    for (int v = 1; v < NUM_VOICES; v++) if (voices[v].active) voiceStop(v);
    startVoice(0, note, holdMs, tag, true);
  }
}

// Start a note. holdMs = 0 holds until voiceOff(); otherwise it stops by itself.
static void voiceOn(uint8_t note, uint32_t holdMs, const char *tag) {
  if (holdMs == 0) heldPush(note);

  if (VOICE_MODE != 0) {          // mono or unison: newest note wins
    startGroup(note, holdMs, tag);
    return;
  }

  // ---- poly ----
  int v = -1;
  // 1) same note already sounding -> restart it on the same motor
  for (int i = 0; i < NUM_VOICES && v < 0; i++)
    if (voices[i].active && voices[i].note == note) v = i;
  // 2) otherwise any free motor
  for (int i = 0; i < NUM_VOICES && v < 0; i++)
    if (!voices[i].active) v = i;
  // 3) otherwise take over the motor that has been playing the longest
  if (v < 0) {
    v = 0;
    for (int i = 1; i < NUM_VOICES; i++)
      if (voices[i].startUs < voices[v].startUs) v = i;
  }
  startVoice(v, note, holdMs, tag, true);
}

// A note ended. Poly: that motor goes quiet. Mono/unison: go back to the
// newest key still held, or go quiet if none.
static void voiceRelease(int v, const char *tag) {
  uint8_t note = voices[v].note;
  if (VOICE_MODE != 0) {
    if (heldCount > 0) {
      startGroup(heldNotes[heldCount - 1], 0, VOICE_MODE == 2 ? "UNI <-  " : "MONO <- ");
      return;
    }
    for (int i = 0; i < NUM_VOICES; i++) voiceStop(i);
    if (MIDI_DEBUG) con_printf("%s %-2s%d (%3d)          -> %s off\r\n", tag,
                               NOTE_NAMES[note % 12], note / 12 - 1, note,
                               VOICE_MODE == 2 ? "all motors" : "M1");
    return;
  }
  voiceStop(v);
  if (MIDI_DEBUG) con_printf("%s %-2s%d (%3d)          -> M%d off\r\n", tag,
                             NOTE_NAMES[note % 12], note / 12 - 1, note, v + 1);
}

// Note released: stop whichever motor is playing it (if it wasn't replaced).
static void voiceOff(uint8_t note, const char *tag) {
  heldRemove(note);
  if (VOICE_MODE != 0) {          // mono/unison: motor 1 tells us what's sounding
    if (voices[0].active && voices[0].note == note) voiceRelease(0, tag);
    return;
  }
  for (int v = 0; v < NUM_VOICES; v++)
    if (voices[v].active && voices[v].note == note) voiceRelease(v, tag);
}

static void seqAllNotesOff();   // recorder hook, defined below

static void allVoicesOff() {
  seqAllNotesOff();
  heldCount = 0;
  for (int v = 0; v < NUM_VOICES; v++) voiceStop(v);
  if (MIDI_DEBUG) con_printf("ALL MOTORS OFF\r\n");
}

// Ends timed (keyboard) notes whose length has run out.
static void voiceServiceTimedReleases() {
  uint32_t ms = millis();
  int last = VOICE_MODE != 0 ? 1 : NUM_VOICES;   // mono/unison: check motor 1 only
  for (int v = 0; v < last; v++)
    if (voices[v].active && voices[v].releaseAtMs &&
        (int32_t)(ms - voices[v].releaseAtMs) >= 0)
      voiceRelease(v, "KEY OFF ");
}

// ---------------- recorder / looper (call with stateLock held) ----------------
// One take of note events lives in SRAM. Event times are ms from the take's
// first note. Playback loops: after seqLengthMs it jumps back to the start.
// Recording captures MIDI IN and serial-keyboard notes (not playback itself).
enum SeqState { SEQ_STOPPED, SEQ_RECORDING, SEQ_PLAYING, SEQ_PAUSED };
static const char *const SEQ_STATE_NAMES[] = {"stopped", "recording", "playing", "paused"};
enum SeqButton { SEQ_BTN_RECORD, SEQ_BTN_PLAY, SEQ_BTN_RESET };

struct SeqEvent {
  uint32_t ms;        // time from the start of the take
  uint8_t  status;    // 0x90 note on / 0x80 note off
  uint8_t  note;
  uint8_t  velocity;
};
static constexpr int      SEQ_MAX_EVENTS    = 6000;                 // 8 bytes each = 48 KB
static constexpr uint32_t SEQ_MAX_LENGTH_MS = 60u * 60u * 1000u;    // 1 hour

static SeqEvent          seqEvents[SEQ_MAX_EVENTS];
static int               seqCount    = 0;
static uint32_t          seqLengthMs = 0;           // loop length; 0 = no take
static volatile SeqState seqState    = SEQ_STOPPED;
static int               seqIndex    = 0;           // next event to play
static uint32_t          seqPosMs    = 0;           // position while stopped / paused
static int64_t           seqZeroUs   = 0;           // when position 0 of this pass happened
static uint32_t          seqSounding[4];            // notes playback switched on (128-bit set)
static int64_t           recStartUs  = -1;          // first note of the take being recorded
static uint32_t          recHeld[4];                // notes on in the take being recorded
struct RecPendingOff { uint8_t note; uint32_t atMs; };
static RecPendingOff     recPending[MAX_HELD];      // recorded keyboard notes that end by themselves
static int               recPendingCount = 0;

static bool bitTest(const uint32_t *s, int n) { return (s[n >> 5] >> (n & 31)) & 1u; }
static void bitSet(uint32_t *s, int n)        { s[n >> 5] |= 1u << (n & 31); }
static void bitClear(uint32_t *s, int n)      { s[n >> 5] &= ~(1u << (n & 31)); }
static int  bitCount(const uint32_t *s) {
  return __builtin_popcount(s[0]) + __builtin_popcount(s[1]) +
         __builtin_popcount(s[2]) + __builtin_popcount(s[3]);
}

static uint32_t seqPositionMs() {
  if (seqState != SEQ_PLAYING) return seqPosMs;
  int64_t ms = (esp_timer_get_time() - seqZeroUs) / 1000;
  return ms < 0 ? 0 : ms > seqLengthMs ? seqLengthMs : (uint32_t)ms;
}

// Tell the desktop app (if listening) what the transport is doing.
static void seqPublish() {
  char buf[160];
  int n = snprintf(buf, sizeof(buf),
                   "{\"v\":1,\"event\":\"transport\",\"state\":\"%s\",\"events\":%d,"
                   "\"length_ms\":%lu,\"pos_ms\":%lu}\n",
                   SEQ_STATE_NAMES[seqState], seqCount, (unsigned long)seqLengthMs,
                   (unsigned long)seqPositionMs());
  if (n > 0 && n < (int)sizeof(buf)) uart_write_bytes(CONSOLE_UART, buf, n);
}

// Release only the notes playback is holding (live playing is left alone).
static void seqSilence() {
  for (int n = 0; n < 128; n++)
    if (bitTest(seqSounding, n)) voiceOff((uint8_t)n, "PLAY OFF");
  memset(seqSounding, 0, sizeof(seqSounding));
}

static uint32_t recClockMs() { return (uint32_t)((esp_timer_get_time() - recStartUs) / 1000); }

static void recAppend(uint8_t status, uint8_t note, uint8_t velocity, uint32_t ms) {
  seqEvents[seqCount++] = {ms, status, note, velocity};
}

static void recStop() {
  if (recStartUs >= 0) {
    uint32_t end = recClockMs();
    for (int n = 0; n < 128; n++)               // close notes still held at the end
      if (bitTest(recHeld, n)) recAppend(0x80, (uint8_t)n, 64, end);
    if (seqCount && end <= seqEvents[seqCount - 1].ms) end = seqEvents[seqCount - 1].ms + 1;
    seqLengthMs = end;
  } else {
    seqCount = 0;                               // no notes were played
    seqLengthMs = 0;
  }
  memset(recHeld, 0, sizeof(recHeld));
  recPendingCount = 0;
  seqState = SEQ_STOPPED;
  seqIndex = 0;
  seqPosMs = 0;
  if (seqLengthMs) con_printf("REC stopped - %d events, %.2f s loop\r\n", seqCount, seqLengthMs / 1000.0);
  else             con_printf("REC stopped - no notes were played\r\n");
}

static void recStart() {
  seqSilence();
  seqCount = 0;
  seqLengthMs = 0;
  seqIndex = 0;
  seqPosMs = 0;
  recStartUs = -1;
  memset(recHeld, 0, sizeof(recHeld));
  recPendingCount = 0;
  seqState = SEQ_RECORDING;
  con_printf("REC armed - the take starts on the first note\r\n");
}

static void recNoteOff(uint8_t note) {
  if (seqState != SEQ_RECORDING || !bitTest(recHeld, note)) return;
  recAppend(0x80, note, 64, recClockMs());
  bitClear(recHeld, note);
}

static void recNoteOn(uint8_t note, uint8_t velocity) {
  if (seqState != SEQ_RECORDING) return;
  if (recStartUs < 0) recStartUs = esp_timer_get_time();   // the take starts on its first note
  recNoteOff(note);                                        // a re-press ends the previous one
  // always keep room to close every held note when recording stops
  if (seqCount + 2 + bitCount(recHeld) > SEQ_MAX_EVENTS || recClockMs() >= SEQ_MAX_LENGTH_MS) {
    con_printf("REC memory full - recording stopped\r\n");
    recStop();
    seqPublish();
    return;
  }
  recAppend(0x90, note, velocity ? velocity : 1, recClockMs());
  bitSet(recHeld, note);
}

// Serial-keyboard notes stop by themselves after holdMs (0 = held until space).
static void recKeyNote(uint8_t note, uint32_t holdMs) {
  recNoteOn(note, 100);
  if (seqState != SEQ_RECORDING || holdMs == 0) return;
  uint32_t at = millis() + holdMs;
  for (int i = 0; i < recPendingCount; i++)
    if (recPending[i].note == note) { recPending[i].atMs = at; return; }
  if (recPendingCount == MAX_HELD) {           // full: end the oldest one now
    recNoteOff(recPending[0].note);
    recPendingCount--;
    memmove(recPending, recPending + 1, sizeof(recPending[0]) * recPendingCount);
  }
  recPending[recPendingCount++] = {note, at};
}

// Call regularly: records the note-offs of timed keyboard notes.
static void recService() {
  uint32_t ms = millis();
  for (int i = 0; i < recPendingCount;) {
    if ((int32_t)(ms - recPending[i].atMs) >= 0) {
      recNoteOff(recPending[i].note);
      recPending[i] = recPending[--recPendingCount];
    } else {
      i++;
    }
  }
}

// allVoicesOff() hook: a panic / all-notes-off also ends recorded and played notes.
static void seqAllNotesOff() {
  memset(seqSounding, 0, sizeof(seqSounding));
  for (int n = 0; n < 128; n++) recNoteOff((uint8_t)n);
  recPendingCount = 0;
}

static void seqPlay() {
  if (!seqLengthMs) {
    con_printf("PLAY - nothing recorded yet\r\n");
    return;
  }
  if (seqPosMs >= seqLengthMs) { seqPosMs = 0; seqIndex = 0; }
  seqZeroUs = esp_timer_get_time() - (int64_t)seqPosMs * 1000;
  seqState  = SEQ_PLAYING;
  con_printf("PLAY from %.2f s (loop %.2f s)\r\n", seqPosMs / 1000.0, seqLengthMs / 1000.0);
}

static void seqPause() {
  seqPosMs = seqPositionMs();
  seqState = SEQ_PAUSED;
  seqSilence();
  con_printf("PAUSE at %.2f s\r\n", seqPosMs / 1000.0);
}

static void seqReset() {
  if (seqState == SEQ_RECORDING) recStop();   // keeps what was recorded
  seqSilence();
  seqState = SEQ_STOPPED;
  seqIndex = 0;
  seqPosMs = 0;
  con_printf("RESET - back to the start\r\n");
}

// One press of a transport button (physical, serial key, or desktop app).
static void seqButton(int button) {
  switch (button) {
    case SEQ_BTN_RECORD:
      if (seqState == SEQ_RECORDING) recStop();
      else                           recStart();
      break;
    case SEQ_BTN_PLAY:
      if (seqState == SEQ_RECORDING)    { recStop(); seqPlay(); }
      else if (seqState == SEQ_PLAYING) seqPause();
      else                              seqPlay();
      break;
    case SEQ_BTN_RESET:
      seqReset();
      break;
  }
  seqPublish();
}

static void seqFire(const SeqEvent &e) {
  if (e.status == 0x90 && e.velocity) {
    voiceOn(e.note, 0, "PLAY ON ");
    bitSet(seqSounding, e.note);
  } else if (bitTest(seqSounding, e.note)) {
    voiceOff(e.note, "PLAY OFF");
    bitClear(seqSounding, e.note);
  }
}

// Called every 1 ms while playing: fire due events, loop at the end of the take.
static void seqService() {
  if (seqState != SEQ_PLAYING) return;
  int64_t now  = esp_timer_get_time();
  int64_t loop = (int64_t)seqLengthMs * 1000;
  for (;;) {
    uint32_t pos = (uint32_t)((now - seqZeroUs) / 1000);
    while (seqIndex < seqCount && seqEvents[seqIndex].ms <= pos) seqFire(seqEvents[seqIndex++]);
    if (pos < seqLengthMs) return;
    seqSilence();                     // end of the take: back to the start
    seqIndex  = 0;
    seqZeroUs += loop;
    if (now - seqZeroUs >= loop) seqZeroUs = now;   // fell a whole loop behind: resync
  }
}

// ---------------- MIDI note handling (call with stateLock held) ----------
static void midiNoteOn(uint8_t note, uint8_t velocity) {
  recNoteOn(note, velocity);
  voiceOn(note, 0, "MIDI ON ");
}
static void midiNoteOff(uint8_t note) {
  recNoteOff(note);
  voiceOff(note, "MIDI OFF");
}
static void midiAllOff() { allVoicesOff(); }

// ---------------- MIDI byte parser ----------------
// Running status supported; real-time bytes (clock etc.) and SysEx ignored.
static uint8_t runningStatus = 0;
static uint8_t midiData[2];
static int     midiDataIdx = 0;
static bool    inSysex = false;

static void handleMidiMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  uint8_t type = status & 0xF0;
  int     ch   = (status & 0x0F) + 1;
  if (MIDI_CHANNEL && ch != MIDI_CHANNEL) return;

  xSemaphoreTake(stateLock, portMAX_DELAY);
  switch (type) {
    case 0x90:                                   // Note On
      if (d2 > 0) midiNoteOn(d1, d2);
      else        midiNoteOff(d1);               // velocity 0 = Note Off
      break;
    case 0x80:                                   // Note Off
      midiNoteOff(d1);
      break;
    case 0xB0:                                   // Control Change
      if (d1 == 120 || d1 == 123) midiAllOff();
      break;
    default:
      break;
  }
  xSemaphoreGive(stateLock);
}

static void midiParseByte(uint8_t b) {
  if (b >= 0xF8) return;                         // real-time

  if (b & 0x80) {                                // status byte
    if (b == 0xF0) { inSysex = true;  runningStatus = 0; return; }
    if (b == 0xF7) { inSysex = false; return; }
    inSysex = false;
    if (b >= 0xF0) { runningStatus = 0; return; } // system common: ignore
    runningStatus = b;
    midiDataIdx = 0;
    return;
  }

  if (inSysex || runningStatus == 0) return;     // stray data byte
  midiData[midiDataIdx++] = b;

  uint8_t type   = runningStatus & 0xF0;
  int     needed = (type == 0xC0 || type == 0xD0) ? 1 : 2;
  if (midiDataIdx < needed) return;

  midiDataIdx = 0;
  handleMidiMessage(runningStatus, midiData[0], needed == 2 ? midiData[1] : 0);
}

// ---------------- debug: raw pin activity ----------------
static volatile uint32_t rxEdges = 0;

// 'c' key arms a one-shot capture of the next burst on the RX pin
// (like a tiny logic analyzer: prints how long each LOW / HIGH lasted).
static constexpr int CAP_MAX = 256;
static volatile int64_t capT[CAP_MAX];
static volatile uint8_t capL[CAP_MAX];
static volatile int     capN = 0;
static volatile bool    capArmed = false;

static void IRAM_ATTR rxEdgeIsr(void *) {
  rxEdges = rxEdges + 1;
  if (capArmed && capN < CAP_MAX) {
    int i = capN;
    capT[i] = esp_timer_get_time();
    capL[i] = (REG_READ(GPIO_IN_REG) >> MIDI_RX_PIN) & 1;   // level just after the edge
    capN = i + 1;
  }
}

static void printCapture() {
  int n = capN;
  con_printf("\r\n--- capture: %d edges (1 MIDI bit = 32 us) ---\r\n", n);
  for (int i = 0; i + 1 < n; i++) {
    int64_t us = capT[i + 1] - capT[i];
    con_printf("%c%-4lld(%4.1f) ", capL[i] ? 'H' : 'L', (long long)us, us / 32.0);
    if ((i + 1) % 8 == 0) con_printf("\r\n");
  }
  if (n > 0) con_printf("%c...(idle)\r\n", capL[n - 1] ? 'H' : 'L');
  con_printf("--- end capture ---\r\n\r\n");
}

// 'm' key: send a test Note On C4 out of MIDI_TX_PIN, Note Off 300 ms later
static bool     testNotePending = false;
static uint32_t testNoteMs = 0;

// encoder state (decoded in encIsr further down)
static volatile int32_t encRaw   = 0;   // +1 / -1 per valid quadrature transition
static volatile uint8_t encState = 0;

// ---------------- serial-monitor keyboard ----------------
static int keyToSemitone(char c) {
  switch (c) {
    case 'a': return 0;   case 'w': return 1;   case 's': return 2;
    case 'e': return 3;   case 'd': return 4;   case 'f': return 5;
    case 't': return 6;   case 'g': return 7;   case 'y': return 8;
    case 'h': return 9;   case 'u': return 10;  case 'j': return 11;
    case 'k': return 12;  case 'o': return 13;  case 'l': return 14;
    case 'p': return 15;  case ';': return 16;
    default:  return -1;
  }
}

static void printHelp() {
  con_printf(
      "\r\n=== ESP32 MIDI / Serial Piano - motor voices (ESP-IDF) ===\r\n"
      "    w   e       t   y   u       o   p\r\n"
      "  a   s   d   f   g   h   j   k   l   ;\r\n"
      "  C   D   E   F   G   A   B   C   D   E\r\n"
      "z/x: octave -/+   -/=: length -/+   space: stop   ?: help\r\n"
      "v: vibrato on/off   c: capture next MIDI burst on RX pin   m: send test note out of TX pin\r\n"
      "n: voice mode (poly -> mono -> unison)\r\n"
      "1: record start/stop   2: play/pause (loops)   3: reset (stop + rewind)\r\n"
      "Knob: turn = choose setting, click = edit, turn = change value, click = done\r\n");
  con_printf("Buttons: record=GPIO%d  play/pause=GPIO%d  reset=GPIO%d   take: %d events, %.2f s (%s)\r\n",
             BTN_RECORD_PIN, BTN_PLAY_PIN, BTN_RESET_PIN, seqCount, seqLengthMs / 1000.0,
             SEQ_STATE_NAMES[seqState]);
  con_printf("Vibrato: %s  rate %.1f Hz  depth +/-%.0f cents  delay %lu ms  fade %lu ms  %s\r\n",
             VIBRATO_ENABLED ? "ON" : "OFF", VIBRATO_RATE_HZ, VIBRATO_DEPTH_CENTS,
             (unsigned long)VIBRATO_DELAY_MS, (unsigned long)VIBRATO_FADE_MS,
             VIBRATO_SHAPE == 1 ? "triangle" : "sine");
  con_printf("Motors:");
  for (int v = 0; v < NUM_VOICES; v++) con_printf("  M%d=GPIO%d", v + 1, VOICE_PINS[v]);
  if (MIDI_CHANNEL) con_printf("   MIDI in: GPIO%d   MIDI channel: %d\r\n", MIDI_RX_PIN, MIDI_CHANNEL);
  else              con_printf("   MIDI in: GPIO%d   MIDI channel: all\r\n", MIDI_RX_PIN);
  if (noteMs) con_printf("Octave: %d   Key length: %lu ms\r\n\r\n", octave, (unsigned long)noteMs);
  else        con_printf("Octave: %d   Key length: sustain\r\n\r\n", octave);
}

// call with stateLock held
static void handleKey(char c) {
  c = (char)tolower((unsigned char)c);
  int semi = keyToSemitone(c);
  if (semi >= 0) {
    int midi = 12 * (octave + 1) + semi;   // C4 = MIDI 60
    recKeyNote((uint8_t)midi, noteMs);
    voiceOn((uint8_t)midi, noteMs, "KEY     ");   // noteMs 0 = hold until space
    return;
  }

  switch (c) {
    case '1': seqButton(SEQ_BTN_RECORD); break;   // same as the transport buttons
    case '2': seqButton(SEQ_BTN_PLAY);   break;
    case '3': seqButton(SEQ_BTN_RESET);  break;
    case 'z':
      if (octave > MIN_OCTAVE) octave--;
      con_printf("Octave %d\r\n", octave);
      break;
    case 'x':
      if (octave < MAX_OCTAVE) octave++;
      con_printf("Octave %d\r\n", octave);
      break;
    case '-':
      noteMs = (noteMs >= NOTE_STEP_MS) ? noteMs - NOTE_STEP_MS : 0;
      if (noteMs) con_printf("Length %lu ms\r\n", (unsigned long)noteMs);
      else        con_printf("Length: sustain (space to stop)\r\n");
      break;
    case '=':
    case '+':
      noteMs = noteMs + NOTE_STEP_MS > MAX_NOTE_MS ? MAX_NOTE_MS : noteMs + NOTE_STEP_MS;
      con_printf("Length %lu ms\r\n", (unsigned long)noteMs);
      break;
    case ' ':
      allVoicesOff();         // panic: stops keyboard and stuck MIDI notes
      break;
    case '?':
      printHelp();
      break;
    case 'n':
      VOICE_MODE = (VOICE_MODE + 1) % 3;   // poly -> mono -> unison -> poly
      allVoicesOff();
      con_printf("Voice mode: %s\r\n", VOICE_MODE_NAMES[VOICE_MODE]);
      break;
    case 'i':
      con_printf("A(GPIO%d)=%d  B(GPIO%d)=%d  BTN(GPIO%d)=%d  encRaw=%ld\r\n",
                 ENC_A_PIN, gpio_get_level((gpio_num_t)ENC_A_PIN),
                 ENC_B_PIN, gpio_get_level((gpio_num_t)ENC_B_PIN),
                 ENC_BTN_PIN, gpio_get_level((gpio_num_t)ENC_BTN_PIN), (long)encRaw);
      break;
    case 'v':
      VIBRATO_ENABLED = !VIBRATO_ENABLED;
      con_printf("Vibrato %s\r\n", VIBRATO_ENABLED ? "ON" : "OFF");
      break;
    case 'c':
      capN = 0;
      capArmed = true;
      con_printf("Capture armed - send a MIDI note now...\r\n");
      break;
    case 'm': {
      static const uint8_t on[3] = {0x90, 0x3C, 0x64};
      uart_write_bytes(MIDI_UART, on, 3);
      testNotePending = true;
      testNoteMs = millis();
      con_printf("TEST TX: 90 3C 64 on GPIO%d\r\n", MIDI_TX_PIN);
      break;
    }
    default:
      break;                  // ignore \r, \n, etc.
  }
}

// ---------------- rotary encoder + button settings menu ----------------
// Turn: scroll through settings.  Click: start editing that setting.
// Turn while editing: change the value.  Click again: done editing.
// (No screen yet, so the menu is shown in the serial monitor.)

// --- encoder decoding (GPIO interrupts on A and B, Gray-code state table) ---

static void IRAM_ATTR encIsr(void *) {
  static const int8_t TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint32_t in = REG_READ(GPIO_IN_REG);
  uint8_t  ab = (uint8_t)((((in >> ENC_A_PIN) & 1) << 1) | ((in >> ENC_B_PIN) & 1));
  encState = (uint8_t)(((encState << 2) | ab) & 0x0F);
  encRaw   = encRaw + TABLE[encState];   // invalid (bouncy) transitions add 0
}

static void encoderInit() {
  gpio_config_t io;
  memset(&io, 0, sizeof(io));
  io.pin_bit_mask = (1ULL << ENC_A_PIN) | (1ULL << ENC_B_PIN);
  io.mode         = GPIO_MODE_INPUT;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;
  io.intr_type    = GPIO_INTR_ANYEDGE;
  ESP_ERROR_CHECK(gpio_config(&io));

  memset(&io, 0, sizeof(io));
  io.pin_bit_mask = 1ULL << ENC_BTN_PIN;
  io.mode         = GPIO_MODE_INPUT;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;   // button shorts the pin to GND
  io.intr_type    = GPIO_INTR_DISABLE;    // polled + debounced in software
  ESP_ERROR_CHECK(gpio_config(&io));

  uint32_t in = REG_READ(GPIO_IN_REG);
  encState = (uint8_t)((((in >> ENC_A_PIN) & 1) << 1) | ((in >> ENC_B_PIN) & 1));
  gpio_isr_handler_add((gpio_num_t)ENC_A_PIN, encIsr, nullptr);
  gpio_isr_handler_add((gpio_num_t)ENC_B_PIN, encIsr, nullptr);
}

// Returns whole detent clicks turned since the last call (+ = clockwise).
static int encReadDetents() {
  static int32_t used = 0;
  int32_t raw = encRaw;
  int steps = 0;
  while (raw - used >= ENC_STEPS_PER_DETENT)  { used += ENC_STEPS_PER_DETENT; steps++; }
  while (raw - used <= -ENC_STEPS_PER_DETENT) { used -= ENC_STEPS_PER_DETENT; steps--; }
  return ENC_REVERSE ? -steps : steps;
}

// Debounced push button to GND: true once per press (on the press, not release).
struct Debounced {
  int      pin;
  int      stable;      // accepted level (1 = released)
  int      last;        // most recent raw level
  uint32_t changedMs;   // when the raw level last changed
};

static bool debouncedPress(Debounced &b) {
  int      level = gpio_get_level((gpio_num_t)b.pin);
  uint32_t now   = millis();
  if (level != b.last) {
    b.last      = level;
    b.changedMs = now;
    return false;
  }
  if (level != b.stable && now - b.changedMs >= BTN_DEBOUNCE_MS) {
    b.stable = level;
    return level == 0;                            // pressed = LOW
  }
  return false;
}

static Debounced encButton = {ENC_BTN_PIN, 1, 1, 0};
static bool buttonPressed() { return debouncedPress(encButton); }

// Transport buttons, in SeqButton order (record, play/pause, reset).
static Debounced transportButtons[] = {
  {BTN_RECORD_PIN, 1, 1, 0}, {BTN_PLAY_PIN, 1, 1, 0}, {BTN_RESET_PIN, 1, 1, 0}};

static void transportInit() {
  gpio_config_t io;
  memset(&io, 0, sizeof(io));
  io.pin_bit_mask = (1ULL << BTN_RECORD_PIN) | (1ULL << BTN_PLAY_PIN) | (1ULL << BTN_RESET_PIN);
  io.mode         = GPIO_MODE_INPUT;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;           // button shorts the pin to GND
  io.intr_type    = GPIO_INTR_DISABLE;            // polled + debounced in software
  ESP_ERROR_CHECK(gpio_config(&io));
}

// Call regularly (every ~10 ms) with stateLock held.
static void transportService() {
  for (int i = 0; i < 3; i++)
    if (debouncedPress(transportButtons[i])) seqButton(i);
}

// --- the settings list: add a row here to put a new setting on the knob ---
enum ParamType { P_BOOL, P_INT, P_U32, P_FLOAT, P_CHOICE };
struct Param {
  const char        *name;
  ParamType          type;
  void              *ptr;
  float              minV, maxV, step;
  const char        *unit;
  const char *const *choices;   // only for P_CHOICE
};

static const char *const SHAPE_NAMES[] = {"sine", "triangle"};

static const Param PARAMS[] = {
  {"Voice mode",       P_CHOICE, &VOICE_MODE,          0,    2,    1,    "",      VOICE_MODE_NAMES},
  {"Unison detune",    P_FLOAT,  &UNISON_DETUNE_CENTS, 0,    50,   1,    "cents", nullptr},
  {"Vibrato",          P_BOOL,   &VIBRATO_ENABLED,     0,    1,    1,    "",      nullptr},
  {"Vibrato rate",     P_FLOAT,  &VIBRATO_RATE_HZ,     0.5f, 15,   0.1f, "Hz",    nullptr},
  {"Vibrato depth",    P_FLOAT,  &VIBRATO_DEPTH_CENTS, 0,    200,  5,    "cents", nullptr},
  {"Vibrato delay",    P_U32,    &VIBRATO_DELAY_MS,    0,    2000, 25,   "ms",    nullptr},
  {"Vibrato fade-in",  P_U32,    &VIBRATO_FADE_MS,     0,    2000, 25,   "ms",    nullptr},
  {"Vibrato shape",    P_CHOICE, &VIBRATO_SHAPE,       0,    1,    1,    "",      SHAPE_NAMES},
  {"MIDI channel",     P_INT,    &MIDI_CHANNEL,        0,    16,   1,    "(0=all)", nullptr},
  {"Key octave",       P_INT,    &octave,              MIN_OCTAVE, MAX_OCTAVE, 1, "", nullptr},
  {"Key note length",  P_U32,    &noteMs,              0,    MAX_NOTE_MS, NOTE_STEP_MS, "ms (0=sustain)", nullptr},
};
static constexpr int NUM_PARAMS = sizeof(PARAMS) / sizeof(PARAMS[0]);

static int  menuIndex = 0;
static bool menuEditing = false;

static void paramAdjust(const Param &p, int steps) {
  switch (p.type) {
    case P_BOOL: {
      bool *b = (bool *)p.ptr;
      if (steps % 2) *b = !*b;
      break;
    }
    case P_INT:
    case P_CHOICE: {
      int *v = (int *)p.ptr;
      int  n = *v + steps * (int)p.step;
      *v = n < (int)p.minV ? (int)p.minV : n > (int)p.maxV ? (int)p.maxV : n;
      break;
    }
    case P_U32: {
      uint32_t *v = (uint32_t *)p.ptr;
      int64_t   n = (int64_t)*v + (int64_t)steps * (int64_t)p.step;
      *v = (uint32_t)(n < (int64_t)p.minV ? (int64_t)p.minV : n > (int64_t)p.maxV ? (int64_t)p.maxV : n);
      break;
    }
    case P_FLOAT: {
      float *v = (float *)p.ptr;
      float  n = *v + steps * p.step;
      n = roundf(n / p.step) * p.step;            // keep it on the step grid
      *v = n < p.minV ? p.minV : n > p.maxV ? p.maxV : n;
      break;
    }
  }
}

static void paramPrint(int i) {
  const Param &p = PARAMS[i];
  char val[24];
  switch (p.type) {
    case P_BOOL:   snprintf(val, sizeof(val), "%s", *(bool *)p.ptr ? "ON" : "OFF"); break;
    case P_INT:    snprintf(val, sizeof(val), "%d", *(int *)p.ptr); break;
    case P_U32:    snprintf(val, sizeof(val), "%lu", (unsigned long)*(uint32_t *)p.ptr); break;
    case P_FLOAT:  snprintf(val, sizeof(val), p.step < 1 ? "%.1f" : "%.0f", *(float *)p.ptr); break;
    case P_CHOICE: snprintf(val, sizeof(val), "%s", p.choices[*(int *)p.ptr]); break;
  }
  con_printf("%s %d/%d  %-16s %s %s\r\n", menuEditing ? "[EDIT]" : "  >   ",
             i + 1, NUM_PARAMS, p.name, val, p.unit);
}

// Call regularly (every ~10 ms) with stateLock held.
static void knobService() {
  if (buttonPressed()) {
    menuEditing = !menuEditing;
    paramPrint(menuIndex);
  }
  int steps = encReadDetents();
  if (steps == 0) return;
  if (menuEditing) {
    int oldMode = VOICE_MODE;
    paramAdjust(PARAMS[menuIndex], steps);
    if (VOICE_MODE != oldMode) allVoicesOff();   // don't leave notes stuck on other motors
  } else {
    menuIndex = ((menuIndex + steps) % NUM_PARAMS + NUM_PARAMS) % NUM_PARAMS;  // wrap around
  }
  paramPrint(menuIndex);
}

// ---------------- desktop app link (JSON lines on the USB serial port) -------
// A line starting with '{' is a request from desktop_app (see
// desktop_app/FIRMWARE_INTERFACE.md); every other byte is a keyboard key.
// Requests are flat JSON objects, so a tiny field finder is enough.
// Everything here runs in consoleTask only.
static constexpr int      REQ_LINE_MAX        = 1024;
static constexpr int      XFER_CHUNK      = 384;          // max bytes per read_chunk
static constexpr size_t   SEQ_MAX_FILE    = 64 * 1024;    // largest .mid the board accepts
static constexpr uint32_t XFER_TIMEOUT_MS = 10000;        // drop half-finished transfers

static uint8_t *readSnap  = nullptr;     // frozen .mid copy of the take being downloaded
static size_t   readSize  = 0;
static char     readTake[12] = "";
static uint8_t *writeBuf  = nullptr;     // staged upload (old take plays until commit)
static size_t   writeSize = 0, writeGot = 0;
static char     writeTake[12] = "";
static uint32_t xferTouchedMs = 0;
static uint32_t takeCounter = 0;

static void xferDropRead()  { free(readSnap); readSnap = nullptr; readTake[0] = 0; }
static void xferDropWrite() { free(writeBuf); writeBuf = nullptr; writeTake[0] = 0; }

// --- JSON fields ---
static const char *jsonFind(const char *js, const char *key) {
  char pat[24];
  int  n = snprintf(pat, sizeof(pat), "\"%s\"", key);
  for (const char *p = strstr(js, pat); p; p = strstr(p + n, pat)) {
    const char *q = p + n;
    while (*q == ' ') q++;
    if (*q != ':') continue;                     // this was a value, not a key
    q++;
    while (*q == ' ') q++;
    return q;
  }
  return nullptr;
}

// Plain string value (no escapes or control characters allowed).
static bool jsonStrRef(const char *js, const char *key, const char **s, size_t *len) {
  const char *v = jsonFind(js, key);
  if (!v || *v != '"') return false;
  const char *e = ++v;
  for (; *e && *e != '"'; e++)
    if (*e == '\\' || (unsigned char)*e < 0x20) return false;
  if (*e != '"') return false;
  *s   = v;
  *len = (size_t)(e - v);
  return true;
}

static bool jsonStr(const char *js, const char *key, char *out, size_t cap) {
  const char *s;
  size_t      len;
  if (!jsonStrRef(js, key, &s, &len) || len >= cap) return false;
  memcpy(out, s, len);
  out[len] = 0;
  return true;
}

static bool jsonInt(const char *js, const char *key, long *out) {
  const char *v = jsonFind(js, key);
  if (!v) return false;
  char *end;
  long  x = strtol(v, &end, 10);
  if (end == v) return false;
  *out = x;
  return true;
}

// --- base64 ---
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64Encode(const uint8_t *in, int n, char *out) {
  int o = 0;
  for (int i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? B64[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

static int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Returns decoded byte count, or -1 if the text is invalid or won't fit.
static int b64Decode(const char *in, size_t n, uint8_t *out, size_t cap) {
  if (n == 0 || n % 4) return -1;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    bool last = i + 4 == n;
    int  a = b64Value(in[i]), b = b64Value(in[i + 1]);
    int  c = (last && in[i + 2] == '=') ? -2 : b64Value(in[i + 2]);
    int  d = (last && in[i + 3] == '=') ? -2 : b64Value(in[i + 3]);
    if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2)) return -1;
    uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)(c < 0 ? 0 : c) << 6 | (d < 0 ? 0 : d);
    int bytes = c == -2 ? 1 : d == -2 ? 2 : 3;
    if (o + bytes > cap) return -1;
    out[o++] = (uint8_t)(v >> 16);
    if (bytes > 1) out[o++] = (uint8_t)(v >> 8);
    if (bytes > 2) out[o++] = (uint8_t)v;
  }
  return (int)o;
}

// --- Standard MIDI File <-> take ---
// Written files: format 0, one track, 480 ticks per beat at 480000 us per
// beat, so 1 tick = 1 ms. End of Track sits at the loop length.
static const uint8_t SMF_HEAD[]  = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                    'M', 'T', 'r', 'k'};
static const uint8_t SMF_TEMPO[] = {0x00, 0xFF, 0x51, 0x03, 0x07, 0x53, 0x00};
static const uint8_t SMF_END[]   = {0xFF, 0x2F, 0x00};

static int vlqLen(uint32_t x) {
  int n = 1;
  while (x >>= 7) n++;
  return n;
}

static uint8_t *vlqPut(uint8_t *p, uint32_t x) {
  uint8_t tmp[5];
  int     n = 0;
  tmp[n++] = x & 0x7F;
  while (x >>= 7) tmp[n++] = 0x80 | (x & 0x7F);
  while (n) *p++ = tmp[--n];
  return p;
}

static bool vlqGet(const uint8_t *&p, const uint8_t *end, uint32_t *out) {
  uint32_t x = 0;
  for (int i = 0; i < 4 && p < end; i++) {
    uint8_t b = *p++;
    x = (x << 7) | (b & 0x7F);
    if (!(b & 0x80)) { *out = x; return true; }
  }
  return false;
}

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// call with stateLock held
static size_t smfTrackLen() {
  size_t   len  = sizeof(SMF_TEMPO) + sizeof(SMF_END);
  uint32_t last = 0;
  for (int i = 0; i < seqCount; i++) {
    len += vlqLen(seqEvents[i].ms - last) + 3;
    last = seqEvents[i].ms;
  }
  return len + vlqLen(seqLengthMs - last);
}

static size_t smfSize() { return sizeof(SMF_HEAD) + 4 + smfTrackLen(); }

// call with stateLock held; p must have room for smfSize() bytes
static void smfEncode(uint8_t *p) {
  uint32_t trk = (uint32_t)smfTrackLen();
  memcpy(p, SMF_HEAD, sizeof(SMF_HEAD));
  p += sizeof(SMF_HEAD);
  *p++ = trk >> 24; *p++ = trk >> 16; *p++ = trk >> 8; *p++ = trk;
  memcpy(p, SMF_TEMPO, sizeof(SMF_TEMPO));
  p += sizeof(SMF_TEMPO);
  uint32_t last = 0;
  for (int i = 0; i < seqCount; i++) {
    const SeqEvent &e = seqEvents[i];
    p    = vlqPut(p, e.ms - last);
    *p++ = e.status;
    *p++ = e.note;
    *p++ = e.velocity;
    last = e.ms;
  }
  p = vlqPut(p, seqLengthMs - last);
  memcpy(p, SMF_END, sizeof(SMF_END));
}

// Reads a format-0 (or single-track format-1) MIDI file with any tick rate and
// tempo changes. Keeps note on/off; skips everything else. Returns nullptr on
// success or an error message. With out == nullptr it only validates + counts.
static const char *smfDecode(const uint8_t *d, size_t n, SeqEvent *out, int *countOut,
                             uint32_t *lengthOut) {
  static char err[96];
  const char *TRUNC = "Truncated MIDI file";
  if (n < 14 || memcmp(d, "MThd", 4) != 0 || be32(d + 4) < 6 || be32(d + 4) > n - 8)
    return "Not a MIDI file";
  uint16_t format = be16(d + 8), tracks = be16(d + 10), division = be16(d + 12);
  if (division & 0x8000) return "SMPTE-timed MIDI files are not supported";
  if (division == 0) return "Bad MIDI header";
  if (format == 2 || tracks != 1)
    return "The synth stores single-track MIDI (the desktop app converts files automatically)";

  const uint8_t *p   = d + 8 + be32(d + 4);
  const uint8_t *end = d + n;
  for (;;) {                                     // find the track (skip unknown chunks)
    if (end - p < 8) return "MIDI file has no track";
    uint32_t len = be32(p + 4);
    if (len > (size_t)(end - p - 8)) return TRUNC;
    if (memcmp(p, "MTrk", 4) == 0) { end = p + 8 + len; p += 8; break; }
    p += 8 + len;
  }

  uint64_t tick = 0, tempoTick = 0, tempoUs = 0;
  uint32_t tempo = 500000;                       // MIDI default: 120 bpm
  uint8_t  running = 0;
  int      count = 0;
  uint32_t lastMs = 0, endMs = 0;
  bool     ended = false;
  while (p < end && !ended) {
    uint32_t delta;
    if (!vlqGet(p, end, &delta)) return TRUNC;
    tick += delta;
    uint64_t ms64 = (tempoUs + (tick - tempoTick) * tempo / division) / 1000;
    if (ms64 > SEQ_MAX_LENGTH_MS) return "Recording is longer than 1 hour";
    uint32_t ms = (uint32_t)ms64;
    if (p >= end) return TRUNC;
    uint8_t status = *p;
    if (status & 0x80)  p++;
    else if (!running)  return "Bad MIDI running status";
    else                status = running;

    if (status == 0xFF || status == 0xF0 || status == 0xF7) {   // meta / SysEx
      uint8_t type = 0;
      if (status == 0xFF) {
        if (p >= end) return TRUNC;
        type = *p++;
      }
      uint32_t len;
      if (!vlqGet(p, end, &len) || len > (size_t)(end - p)) return TRUNC;
      if (status == 0xFF && type == 0x51 && len == 3) {         // tempo change
        uint32_t t = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        if (!t) return "Bad MIDI tempo";
        tempoUs   = tempoUs + (tick - tempoTick) * tempo / division;
        tempoTick = tick;
        tempo     = t;
      } else if (status == 0xFF && type == 0x2F) {              // end of track
        ended = true;
        endMs = ms;
      }
      p += len;
      running = 0;
      continue;
    }
    if (status >= 0xF0) return "Unsupported MIDI event";

    running = status;
    uint8_t type = status & 0xF0;
    int     need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
    if (end - p < need) return TRUNC;
    if ((p[0] & 0x80) || (need == 2 && (p[1] & 0x80))) return "Bad MIDI data byte";
    if (type == 0x80 || type == 0x90) {
      if (count == SEQ_MAX_EVENTS) {
        snprintf(err, sizeof(err), "Too many notes for the synth (max %d note on/off events)",
                 SEQ_MAX_EVENTS);
        return err;
      }
      if (out) out[count] = {ms, (uint8_t)((type == 0x90 && p[1]) ? 0x90 : 0x80), p[0], p[1]};
      count++;
      lastMs = ms;
    }
    p += need;
  }
  if (!ended) return "MIDI track has no End of Track";
  if (!count) return "No notes in this MIDI file";
  *countOut  = count;
  *lengthOut = endMs > lastMs ? endMs : lastMs + 1;
  return nullptr;
}

// --- replies ---
static char replyBuf[XFER_CHUNK * 4 / 3 + 160];

static void replyOk(const char *id, const char *fields = "") {
  int n = snprintf(replyBuf, sizeof(replyBuf), "{\"v\":1,\"id\":\"%s\",\"ok\":true%s}\n", id, fields);
  if (n > 0 && n < (int)sizeof(replyBuf)) uart_write_bytes(CONSOLE_UART, replyBuf, n);
}

static void replyError(const char *id, const char *error) {
  int n = snprintf(replyBuf, sizeof(replyBuf), "{\"v\":1,\"id\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n",
                   id, error);
  if (n > 0 && n < (int)sizeof(replyBuf)) uart_write_bytes(CONSOLE_UART, replyBuf, n);
}

static bool takeMatches(const char *line, const char *expected) {
  char take[12];
  return expected[0] && jsonStr(line, "take", take, sizeof(take)) && strcmp(take, expected) == 0;
}

static void handleRequest(const char *line) {
  char id[33], op[20];
  long v = 0;
  if (!jsonInt(line, "v", &v) || v != 1 || !jsonStr(line, "id", id, sizeof(id))) return;  // not ours
  if (!jsonStr(line, "op", op, sizeof(op))) { replyError(id, "Missing op"); return; }
  char fields[96];

  if (strcmp(op, "hello") == 0) {
    xferDropRead();
    xferDropWrite();
    snprintf(fields, sizeof(fields),
             ",\"caps\":[\"midi_file\",\"transport\"],\"max_events\":%d,\"max_file\":%u",
             SEQ_MAX_EVENTS, (unsigned)SEQ_MAX_FILE);
    replyOk(id, fields);
    xSemaphoreTake(stateLock, portMAX_DELAY);
    seqPublish();
    xSemaphoreGive(stateLock);
    return;
  }
  if (strcmp(op, "ping") == 0) { replyOk(id); return; }

  int button = strcmp(op, "record") == 0 ? SEQ_BTN_RECORD
             : strcmp(op, "play") == 0   ? SEQ_BTN_PLAY
             : strcmp(op, "reset") == 0  ? SEQ_BTN_RESET : -1;
  if (button >= 0 || strcmp(op, "status") == 0) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    if (button >= 0) seqButton(button);          // publishes the new state
    else             seqPublish();
    xSemaphoreGive(stateLock);
    replyOk(id);
    return;
  }

  if (strcmp(op, "read_begin") == 0) {
    xferDropRead();
    const char *err = nullptr;
    xSemaphoreTake(stateLock, portMAX_DELAY);
    if (seqState == SEQ_RECORDING) err = "Stop recording first";
    else if (!seqLengthMs)         err = "No recording on the synth yet";
    else {
      readSize = smfSize();
      readSnap = (uint8_t *)malloc(readSize);
      if (readSnap) smfEncode(readSnap);
      else          err = "Not enough free SRAM to send the recording";
    }
    xSemaphoreGive(stateLock);
    if (err) { replyError(id, err); return; }
    snprintf(readTake, sizeof(readTake), "r%lu", (unsigned long)++takeCounter);
    xferTouchedMs = millis();
    snprintf(fields, sizeof(fields), ",\"take\":\"%s\",\"size\":%u", readTake, (unsigned)readSize);
    replyOk(id, fields);
    return;
  }

  if (strcmp(op, "read_chunk") == 0) {
    long off, len;
    if (!readSnap || !takeMatches(line, readTake)) { replyError(id, "Unknown take"); return; }
    if (!jsonInt(line, "offset", &off) || !jsonInt(line, "length", &len) || off < 0 || len < 1 ||
        len > XFER_CHUNK || (size_t)(off + len) > readSize) {
      replyError(id, "Bad chunk range");
      return;
    }
    xferTouchedMs = millis();
    static char data[XFER_CHUNK * 4 / 3 + 24];
    int n = snprintf(data, sizeof(data), ",\"data\":\"");
    n += b64Encode(readSnap + off, (int)len, data + n);
    snprintf(data + n, sizeof(data) - n, "\"");
    replyOk(id, data);
    return;
  }

  if (strcmp(op, "write_begin") == 0) {
    long size;
    xferDropWrite();
    if (!jsonInt(line, "size", &size) || size < 14 || (size_t)size > SEQ_MAX_FILE) {
      snprintf(fields, sizeof(fields), "MIDI file must be at most %u KB for the synth",
               (unsigned)(SEQ_MAX_FILE / 1024));
      replyError(id, fields);
      return;
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < (size_t)size + 16384 ||
        !(writeBuf = (uint8_t *)malloc(size))) {
      replyError(id, "Not enough free SRAM for this file");
      return;
    }
    writeSize = (size_t)size;
    writeGot  = 0;
    snprintf(writeTake, sizeof(writeTake), "w%lu", (unsigned long)++takeCounter);
    xferTouchedMs = millis();
    snprintf(fields, sizeof(fields), ",\"take\":\"%s\"", writeTake);
    replyOk(id, fields);
    return;
  }

  if (strcmp(op, "write_chunk") == 0) {
    long        off;
    const char *data;
    size_t      dlen;
    if (!writeBuf || !takeMatches(line, writeTake)) { replyError(id, "Unknown take"); return; }
    if (!jsonInt(line, "offset", &off) || off != (long)writeGot) { replyError(id, "Chunk out of order"); return; }
    int n = jsonStrRef(line, "data", &data, &dlen)
                ? b64Decode(data, dlen, writeBuf + writeGot, writeSize - writeGot) : -1;
    if (n <= 0) { replyError(id, "Bad chunk data"); return; }
    writeGot += (size_t)n;
    xferTouchedMs = millis();
    replyOk(id);
    return;
  }

  if (strcmp(op, "write_commit") == 0) {
    if (!writeBuf || !takeMatches(line, writeTake)) { replyError(id, "Unknown take"); return; }
    int         count  = 0;
    uint32_t    length = 0;
    const char *err    = writeGot != writeSize ? "Upload incomplete"
                       : smfDecode(writeBuf, writeSize, nullptr, &count, &length);
    if (!err) {
      xSemaphoreTake(stateLock, portMAX_DELAY);
      if (seqState == SEQ_RECORDING) {
        err = "Stop recording first";
      } else {                                   // swap the new take in
        seqSilence();
        smfDecode(writeBuf, writeSize, seqEvents, &seqCount, &seqLengthMs);
        seqState = SEQ_STOPPED;
        seqIndex = 0;
        seqPosMs = 0;
        con_printf("LOADED from desktop - %d events, %.2f s loop (press play)\r\n", seqCount,
                   seqLengthMs / 1000.0);
        seqPublish();
      }
      xSemaphoreGive(stateLock);
    }
    xferDropWrite();
    if (err) { replyError(id, err); return; }
    snprintf(fields, sizeof(fields), ",\"events\":%d,\"length_ms\":%lu", count, (unsigned long)length);
    replyOk(id, fields);
    return;
  }

  replyError(id, "Unsupported request");
}

// ---------------- tasks ----------------
static QueueHandle_t midiUartQueue;
static TaskHandle_t  seqTaskHandle;

// 1 ms tick (FreeRTOS ticks are only 10 ms here) wakes the sequencer while playing.
static void seqTickCb(void *) {
  if (seqState == SEQ_PLAYING) xTaskNotifyGive(seqTaskHandle);
}

static void seqTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    xSemaphoreTake(stateLock, portMAX_DELAY);
    seqService();
    xSemaphoreGive(stateLock);
  }
}

// MIDI: wait for UART events (data or errors) from the driver.
static void midiTask(void *) {
  uart_event_t ev;
  uint8_t buf[128];
  for (;;) {
    if (!xQueueReceive(midiUartQueue, &ev, portMAX_DELAY)) continue;

    switch (ev.type) {
      case UART_DATA: {
        int n = uart_read_bytes(MIDI_UART, buf, ev.size < sizeof(buf) ? ev.size : sizeof(buf), 0);
        if (RAW_DEBUG && n > 0) {
          char line[3 * 128 + 16];
          int pos = snprintf(line, sizeof(line), "RX %2d:", n);
          for (int i = 0; i < n && pos < (int)sizeof(line) - 4; i++)
            pos += snprintf(line + pos, sizeof(line) - pos, " %02X", buf[i]);
          con_printf("%s\r\n", line);
        }
        for (int i = 0; i < n; i++) midiParseByte(buf[i]);
        break;
      }
      case UART_FRAME_ERR:
        if (RAW_DEBUG) con_printf("UART framing error  (bad baud, inverted signal, or noisy line)\r\n");
        break;
      case UART_PARITY_ERR:
        if (RAW_DEBUG) con_printf("UART parity error\r\n");
        break;
      case UART_BREAK:
        if (RAW_DEBUG) con_printf("UART break  (RX line held LOW - signal inverted or missing pull-up?)\r\n");
        break;
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        if (RAW_DEBUG) con_printf("UART overflow - flushing\r\n");
        uart_flush_input(MIDI_UART);
        xQueueReset(midiUartQueue);
        break;
      default:
        break;
    }
  }
}

// Console: keys + desktop-app requests, keyboard note auto-release, knob, buttons.
static void consoleTask(void *) {
  static uint8_t buf[256];
  static char    line[REQ_LINE_MAX];
  int      lineLen = -1;            // -1 = not inside a desktop-app request line
  bool     lineTooLong = false;
  uint32_t lastPinReport = 0;
  for (;;) {
    int n = uart_read_bytes(CONSOLE_UART, buf, sizeof(buf), pdMS_TO_TICKS(10));

    if (PIN_MONITOR && millis() - lastPinReport >= 2000) {
      lastPinReport = millis();
      uint32_t edges = rxEdges;
      rxEdges = 0;
      con_printf("[pin D%d] level=%d  edges(2s)=%lu%s\r\n", MIDI_RX_PIN,
                 gpio_get_level((gpio_num_t)MIDI_RX_PIN), (unsigned long)edges,
                 gpio_get_level((gpio_num_t)MIDI_RX_PIN) ? "" : "   <-- idle should be 1!");
    }

    // finish a capture once the line has been quiet for 5 ms (or buffer full)
    if (capArmed && capN > 0 &&
        (capN >= CAP_MAX || esp_timer_get_time() - capT[capN - 1] > 5000)) {
      capArmed = false;
      printCapture();
    }

    if (testNotePending && millis() - testNoteMs >= 300) {
      static const uint8_t off[3] = {0x80, 0x3C, 0x00};
      uart_write_bytes(MIDI_UART, off, 3);
      testNotePending = false;
      con_printf("TEST TX: 80 3C 00\r\n");
    }

    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (lineLen >= 0) {                        // inside a desktop-app request
        if (c == '\n' || c == '\r') {
          line[lineLen] = 0;
          if (!lineTooLong) handleRequest(line);
          lineLen = -1;
        } else if (lineLen < REQ_LINE_MAX - 1) {
          line[lineLen++] = c;
        } else {
          lineTooLong = true;
        }
      } else if (c == '{') {
        line[0]     = c;
        lineLen     = 1;
        lineTooLong = false;
      } else {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        handleKey(c);
        xSemaphoreGive(stateLock);
      }
    }

    if ((readSnap || writeBuf) && millis() - xferTouchedMs > XFER_TIMEOUT_MS) {
      xferDropRead();              // desktop went away mid-transfer
      xferDropWrite();
    }

    xSemaphoreTake(stateLock, portMAX_DELAY);
    voiceServiceTimedReleases();   // ends keyboard notes after their length
    recService();                  // records those note-offs too
    knobService();                 // rotary encoder settings menu
    transportService();            // record / play / reset buttons
    xSemaphoreGive(stateLock);
  }
}

// ---------------- entry point ----------------
extern "C" void app_main(void) {
  stateLock = xSemaphoreCreateMutex();

  // Console UART0 (already running at 115200 from boot; just add a driver for RX)
  ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 1024, 0, 0, nullptr, 0));

  // MIDI UART2 @ 31250 8N1, RX only
  uart_config_t mc;
  memset(&mc, 0, sizeof(mc));
  mc.baud_rate  = 31250;
  mc.data_bits  = UART_DATA_8_BITS;
  mc.parity     = UART_PARITY_DISABLE;
  mc.stop_bits  = UART_STOP_BITS_1;
  mc.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  mc.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_param_config(MIDI_UART, &mc));
  ESP_ERROR_CHECK(uart_set_pin(MIDI_UART, MIDI_TX_PIN, MIDI_RX_PIN,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_driver_install(MIDI_UART, 1024, 0, 20, &midiUartQueue, 0));
  // weak internal pull-up so the line idles HIGH even without an external resistor
  gpio_set_pull_mode((gpio_num_t)MIDI_RX_PIN, GPIO_PULLUP_ONLY);

  // count every edge on the RX pin, independent of the UART ('c' capture + pin monitor)
  gpio_install_isr_service(0);
  gpio_set_intr_type((gpio_num_t)MIDI_RX_PIN, GPIO_INTR_ANYEDGE);
  gpio_isr_handler_add((gpio_num_t)MIDI_RX_PIN, rxEdgeIsr, nullptr);

  encoderInit();                  // rotary encoder A/B + button (pins set at top)
  transportInit();                // record / play / reset buttons

  toneInit();
  pitchEngineStart();
  printHelp();

  xTaskCreate(midiTask,    "midi",    4096, nullptr, 10, nullptr);
  xTaskCreate(seqTask,     "seq",     4096, nullptr, 8,  &seqTaskHandle);
  xTaskCreate(consoleTask, "console", 6144, nullptr, 5,  nullptr);

  esp_timer_create_args_t args;
  memset(&args, 0, sizeof(args));
  args.callback = seqTickCb;
  args.name     = "seq";
  esp_timer_handle_t seqTimer;
  ESP_ERROR_CHECK(esp_timer_create(&args, &seqTimer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(seqTimer, 1000));
}