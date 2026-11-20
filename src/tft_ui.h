#pragma once
#include <stdint.h>

// Display driver assumes 320x240 landscape ILI9341 on SPI.
// Change pins in tft_ui.cpp if required by your wiring.
struct TftUiState {
  int selected;
  bool editing;
  int activeVoices;
  int recordingState; // 0 stopped, 1 recording, 2 playing, 3 paused
  bool hasRecording;
  int attackMs, decayMs, sustainPct, releaseMs;
  int otherCount;
  const char *otherNames[16];
  char otherValues[16][24];
};
void tftUiInit();
void tftUiDraw(const TftUiState &state);
