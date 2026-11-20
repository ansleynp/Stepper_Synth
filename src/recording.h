#pragma once
#include <stdint.h>
#include <stddef.h>

namespace recording {
constexpr size_t MAX_EVENTS = 2048;
constexpr size_t MAX_FILE_BYTES = 16384;
struct Event { uint32_t ms; uint8_t note, velocity; bool on; };
enum Mode { IDLE, RECORDING, PLAYING };
void start(uint32_t now);
void stop(uint32_t now=0);
void play(uint32_t now);
void reset();
Mode mode();
size_t count();
void capture(uint32_t now, uint8_t note, uint8_t velocity, bool on);
// Returns one due event at a time. Call with a single lock covering capture + playback.
bool next(uint32_t now, Event &out);
// MIDI serialization is 480 PPQ, 120 BPM, format 0, single track.
// Transfer functions must only run while IDLE, under the same lock.
bool exportMidi(const uint8_t *&data, size_t &size);
bool importMidi(const uint8_t *data, size_t size);
}
