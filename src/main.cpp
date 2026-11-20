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
 */

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "recording.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "driver/gpio.h"
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
static constexpr int        VOICE_PINS[]  = {22, 21, 19};
static constexpr int        NUM_VOICES    = sizeof(VOICE_PINS) / sizeof(VOICE_PINS[0]);
static_assert(NUM_VOICES >= 1 && NUM_VOICES <= 4, "LEDC low-speed mode has 4 timers");
static constexpr int        MIDI_RX_PIN   = 16;          // MIDI data in
static constexpr int        MIDI_TX_PIN   = 17;          // test output for 'm' loopback test
static constexpr int        ENC_A_PIN     = 12;          // rotary encoder A (CLK)
static constexpr int        ENC_B_PIN     = 14;          // rotary encoder B (DT)
static constexpr int        ENC_BTN_PIN   = 13;           // encoder push button (SW), to GND when pressed
static constexpr int        ENC_STEPS_PER_DETENT = 4;    // most encoders: 4 (try 2 if one click = 2 steps)
static constexpr bool       ENC_REVERSE   = false;       // true if clockwise goes the wrong way
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
        cents = VIBRATO_DEPTH_CENTS * fade * lfo(VIBRATO_RATE_HZ * sinceDelay / 1000.0f);
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

static void allVoicesOff() {
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
      {
        recording::capture(ms, voices[v].note, 0, false);
        voiceRelease(v, "KEY OFF ");
      }
}

// ---------------- MIDI note handling (call with stateLock held) ----------
static void midiNoteOn(uint8_t note, uint8_t velocity) {
  if (recording::mode() == recording::PLAYING) return;
  recording::capture(millis(), note, velocity, true);
  voiceOn(note, 0, "MIDI ON ");
}
static void midiNoteOff(uint8_t note) {
  if (recording::mode() == recording::PLAYING) return;
  recording::capture(millis(), note, 0, false);
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
      "1: record start/stop   2: playback start/stop (loop)   3: clear take\r\n"
      "Knob: turn = choose setting, click = edit, turn = change value, click = done\r\n");
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
    if (recording::mode()==recording::PLAYING) return;
    int midi = 12 * (octave + 1) + semi;   // C4 = MIDI 60
    recording::capture(millis(), (uint8_t)midi, 100, true);
    voiceOn((uint8_t)midi, noteMs, "KEY     ");   // noteMs 0 = hold until space
    return;
  }

  switch (c) {
    case '1':
      if(recording::mode()!=recording::RECORDING) {
        recording::stop(millis()); allVoicesOff(); recording::start(millis());
        con_printf("RECORDING started (SRAM)\r\n");
      } else { recording::stop(millis()); con_printf("RECORDING stopped: %u events\r\n", (unsigned)recording::count()); }
      break;
    case '2':
      if(recording::mode()==recording::PLAYING) {
        recording::stop(millis()); allVoicesOff(); con_printf("PLAYBACK stopped\r\n");
      } else if(recording::mode()==recording::IDLE && recording::count()) {
        allVoicesOff(); recording::play(millis()); con_printf("PLAYBACK looping\r\n");
      } else con_printf("Stop recording / record notes before playback\r\n");
      break;
    case '3':
      recording::reset(); allVoicesOff(); con_printf("Recording cleared\r\n"); break;
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
      for(int i=0;i<NUM_VOICES;i++) if(voices[i].active) recording::capture(millis(),voices[i].note,0,false);
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
static volatile int32_t encRaw   = 0;   // +1 / -1 per valid quadrature transition
static volatile uint8_t encState = 0;

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

// Debounced button: returns true once per press (on the press, not release).
static bool buttonPressed() {
  static int  stableLevel = 1, lastLevel = 1;
  static int  sameCount = 0;
  int level = gpio_get_level((gpio_num_t)ENC_BTN_PIN);
  if (level == lastLevel) {
    if (sameCount < 3) sameCount++;
  } else {
    sameCount = 0;
    lastLevel = level;
  }
  if (sameCount == 3 && level != stableLevel) {   // stable for ~30 ms
    stableLevel = level;
    return level == 0;                            // pressed = LOW
  }
  return false;
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

// ---------------- tasks ----------------
static QueueHandle_t midiUartQueue;

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


// USB JSON-lines protocol for the desktop app. Called with stateLock held.
// Do not use the serial monitor and desktop app at the same time (one USB port).
static uint8_t upload[recording::MAX_FILE_BYTES];
static size_t uploadSize=0, uploadAt=0;
static bool uploading=false;
static const uint8_t *download=nullptr;
static size_t downloadSize=0;
static bool downloading=false;
static const char *TAKE_TOKEN="take1";
static void protocolLine(const char *line) {
  cJSON *req=cJSON_Parse(line);
  if(!req)return;
  cJSON *v=cJSON_GetObjectItemCaseSensitive(req,"v");
  cJSON *id=cJSON_GetObjectItemCaseSensitive(req,"id");
  cJSON *op=cJSON_GetObjectItemCaseSensitive(req,"op");
  if(!cJSON_IsNumber(v) || v->valueint!=1 || !cJSON_IsString(id) || !cJSON_IsString(op)) {cJSON_Delete(req);return;}
  cJSON *out=cJSON_CreateObject();
  cJSON_AddNumberToObject(out,"v",1);
  cJSON_AddStringToObject(out,"id",id->valuestring);
  bool ok=true;
  const char *err="Unsupported operation or invalid state";
  const char *cmd=op->valuestring;
  if(!strcmp(cmd,"hello")) {
    cJSON *caps=cJSON_AddArrayToObject(out,"caps");
    cJSON_AddItemToArray(caps,cJSON_CreateString("midi_file"));
  } else if(!strcmp(cmd,"ping")) {
  } else if(!strcmp(cmd,"record_start")) {
    if(recording::mode()==recording::PLAYING) {recording::stop(millis()); allVoicesOff();}
    allVoicesOff(); recording::start(millis());
  } else if(!strcmp(cmd,"record_stop")) {
    if(recording::mode()!=recording::RECORDING) ok=false;
    else recording::stop(millis());
  } else if(!strcmp(cmd,"play_start")) {
    if(recording::mode()!=recording::IDLE || !recording::count()) ok=false;
    else {allVoicesOff();recording::play(millis());}
  } else if(!strcmp(cmd,"play_stop")) {
    recording::stop(millis());allVoicesOff();
  } else if(!strcmp(cmd,"record_clear")) {
    recording::reset();allVoicesOff();
  } else if(!strcmp(cmd,"record_status")) {
    const char *m=recording::mode()==recording::RECORDING?"recording":recording::mode()==recording::PLAYING?"playing":"idle";
    cJSON_AddStringToObject(out,"mode",m);
    cJSON_AddNumberToObject(out,"events",recording::count());
  } else if(!strcmp(cmd,"read_begin")) {
    if(recording::mode()!=recording::IDLE || !recording::exportMidi(download,downloadSize)) ok=false;
    else {downloading=true;cJSON_AddNumberToObject(out,"size",downloadSize);cJSON_AddStringToObject(out,"take",TAKE_TOKEN);}
  } else if(!strcmp(cmd,"read_chunk")) {
    cJSON *take=cJSON_GetObjectItem(req,"take"),*offset=cJSON_GetObjectItem(req,"offset"),*length=cJSON_GetObjectItem(req,"length");
    if(!downloading || !cJSON_IsString(take) || strcmp(take->valuestring,TAKE_TOKEN) ||
       !cJSON_IsNumber(offset)||!cJSON_IsNumber(length)||offset->valuedouble<0||length->valuedouble<1||length->valuedouble>384 ||
       offset->valuedouble!=offset->valueint || length->valuedouble!=length->valueint ||
       size_t(offset->valueint)>downloadSize || size_t(length->valueint)>downloadSize-size_t(offset->valueint))ok=false;
    else {
      unsigned char encoded[520];size_t bytes=0;
      if(mbedtls_base64_encode(encoded,sizeof(encoded),&bytes,download+offset->valueint,length->valueint)!=0)ok=false;
      else {encoded[bytes]=0;cJSON_AddStringToObject(out,"data",(const char *)encoded);}
    }
  } else if(!strcmp(cmd,"write_begin")) {
    cJSON *size=cJSON_GetObjectItem(req,"size");
    if(recording::mode()!=recording::IDLE || !cJSON_IsNumber(size) || size->valuedouble!=size->valueint ||
       size->valueint<26 || size->valueint>int(recording::MAX_FILE_BYTES))ok=false;
    else {uploadSize=size->valueint;uploadAt=0;uploading=true;downloading=false;cJSON_AddStringToObject(out,"take",TAKE_TOKEN);}
  } else if(!strcmp(cmd,"write_chunk")) {
    cJSON *take=cJSON_GetObjectItem(req,"take"),*offset=cJSON_GetObjectItem(req,"offset"),*data=cJSON_GetObjectItem(req,"data");
    if(!uploading||!cJSON_IsString(take)||strcmp(take->valuestring,TAKE_TOKEN)||
       !cJSON_IsNumber(offset)||offset->valuedouble!=offset->valueint||offset->valueint<0||
       size_t(offset->valueint)!=uploadAt||!cJSON_IsString(data))ok=false;
    else {size_t len=0;
      if(mbedtls_base64_decode(upload+uploadAt,uploadSize-uploadAt,&len,(const unsigned char *)data->valuestring,strlen(data->valuestring))!=0)ok=false;
      else uploadAt+=len;
    }
  } else if(!strcmp(cmd,"write_commit")) {
    cJSON *take=cJSON_GetObjectItem(req,"take");
    if(!uploading||!cJSON_IsString(take)||strcmp(take->valuestring,TAKE_TOKEN)||uploadAt!=uploadSize||
       !recording::importMidi(upload,uploadSize))ok=false;
    uploading=false;
  } else ok=false;
  cJSON_AddBoolToObject(out,"ok",ok);
  if(!ok)cJSON_AddStringToObject(out,"error",err);
  char *answer=cJSON_PrintUnformatted(out);
  if(answer){con_printf("%s\r\n",answer);free(answer);}
  cJSON_Delete(out);cJSON_Delete(req);
}
static char jsonLine[1024];
static size_t jsonUsed=0;
static bool inJson=false;
static void consoleInput(uint8_t c) {
  if(!inJson) {
    if(c=='{') {inJson=true;jsonUsed=0;jsonLine[jsonUsed++]='{';}
    else handleKey((char)c);
    return;
  }
  if(c=='\n') {jsonLine[jsonUsed]=0;protocolLine(jsonLine);inJson=false;jsonUsed=0;return;}
  if(c=='\r')return;
  if(jsonUsed+1>=sizeof(jsonLine)) {inJson=false;jsonUsed=0;return;}
  jsonLine[jsonUsed++]=char(c);
}

// Console: poll keys every 10 ms and handle keyboard note auto-release.
static void consoleTask(void *) {
  uint8_t buf[16];
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

    xSemaphoreTake(stateLock, portMAX_DELAY);
    for (int i = 0; i < n; i++) consoleInput(buf[i]);
    recording::Event ev;
    for(int i=0;i<16 && recording::next(millis(),ev);i++) {
      if(ev.on) voiceOn(ev.note,0,"PLAY ON ");
      else voiceOff(ev.note,"PLAY OFF");
    }
    voiceServiceTimedReleases();   // ends keyboard notes after their length
    knobService();                 // rotary encoder settings menu
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

  encoderInit();                  // rotary encoder on GPIO12/14, button on GPIO13

  toneInit();
  pitchEngineStart();
  printHelp();

  xTaskCreate(midiTask,    "midi",    4096, nullptr, 10, nullptr);
  xTaskCreate(consoleTask, "console", 4096, nullptr, 5,  nullptr);
}