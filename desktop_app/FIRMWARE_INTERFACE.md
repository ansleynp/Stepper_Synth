# Desktop interface v1

This is the wire format implemented by this desktop version, for the teammate writing the ESP32 side. The boot-message starter does not implement this interface. Both capabilities can be implemented independently.

## Transport

USB serial at 115200 baud, 8-N-1. Send UTF-8 JSON objects terminated by a newline. Each desktop request includes v (1), id (an opaque string) and op. Each reply echoes v and id and includes ok (boolean). On refusal send ok false and a short error string. The desktop waits up to three seconds per request. End every boot-log line with a newline. Opening the development board's serial connection may reset the ESP32 and clear SRAM. Connect before recording.

Example handshake (reply echoes the actual request ID):

```json
{"v":1,"id":"abc","op":"hello"}
{"v":1,"id":"abc","ok":true,"caps":["chords","midi_file"]}
```

## Live chords

| Request | Board behavior |
| --- | --- |
| hello | Return the supported caps array. Begin each new desktop session in ordinary mode. |
| set_map | first is MIDI 45 (A2). rows contains 40 arrays, one per trigger pitch through MIDI 84 (C6). Each array contains one or three MIDI notes in motor-slot order. Validate all rows and replace the map atomically. Acknowledge after applying. |
| identify | Enable ordinary voice routing and publish held keys plus sounding voices. |
| ordinary | Release generated voices and return to ordinary live playing. |
| ping | Acknowledge. While live, the desktop pings about once per second. On link loss or five seconds without traffic, release generated notes and revert to ordinary mode. |

Auto gives the newest eligible physical key the group of voices. For overlap, maintain held keys in press order, reverting to the most recent still-held key on release. Handle sustain and Stop-All locally. Timing and voice envelopes belong to the board. A new map applies on the next attack. Keep old sounding pitches through their release. Publish status after the mode ACK and on every relevant note, pedal or voice change. Example for C4 in C major:

```json
{"v":1,"event":"voices","root":60,"held":[60],"notes":[60,64,67]}
```

notes holds zero to three sounding pitches in motor order (use contiguous slots in this interface). held lists physical key pitches and may contain more than three. root is the owning trigger in Auto. Publish empty arrays on release to silence. Boot logs may share the port. Use ordinary mode and reject conflicting requests while the synth is recording or playing back.

## Recording exchange

This desktop version exchanges complete Standard MIDI File bytes. The application does not assume that the ESP32's internal event structs are MIDI files. Advertise midi_file only if the firmware can encode and decode this representation. A team firmware design using timestamped events or a companion settings file needs a corresponding adapter. The current transfer includes only what the MIDI file encodes.

| Request | Fields or response |
| --- | --- |
| read_begin | Freeze a consistent take snapshot and return its take token (string) and byte size. |
| read_chunk | Request includes take, byte offset and length (up to 384). Return data as base64 for exactly that slice. |
| write_begin | Request includes size. Check capacity, start a staging area and return a take token. |
| write_chunk | Validate take, offset and decoded bytes. Reply only after staging. |
| write_commit | Validate size, MIDI structure and supported event/range limits, then swap the staged take into SRAM and acknowledge. Return an error if the take cannot be played. |

The desktop transfer limit is 256 KiB. The board should enforce its actual remaining SRAM limit, which may be smaller. On disconnect, error or inactivity, discard incomplete writes and release read snapshots. Keep the old playable take until commit succeeds. The app validates downloaded MIDI before saving a new local file. A transfer timeout at commit leaves the result unconfirmed on the desktop.

## Transport (looper)

Implemented by `src/main.cpp`, which advertises caps `["midi_file","transport"]` plus `max_events` (note on/off events the take can hold) and `max_file` (largest accepted .mid in bytes) in the hello reply. It does not implement the live-chord requests.

| Request | Board behavior |
| --- | --- |
| record | Same as the record button: start a new take, or stop the one being recorded. |
| play | Same as the play button: play / pause. Pressed while recording, it stops recording and plays. |
| reset | Same as the reset button: stop playback (or recording) and rewind to the start. |
| status | Acknowledge and publish a transport event. |

The board publishes a transport event after hello, on every button press (physical, serial key 1/2/3, or desktop request) and after a successful write_commit:

```json
{"v":1,"event":"transport","state":"playing","events":48,"length_ms":5120,"pos_ms":0}
```

state is one of stopped, recording, playing, paused. Desktops should ignore event types they don't know.

Board details:

- A take starts on its first note, so silence after pressing Record is trimmed. It ends when Record (or Play / Reset) is pressed. The loop length runs to that press.
- read_begin is refused while recording or when there is no take. write_commit is refused while recording. A successful commit stops playback and rewinds; press Play to start the new take.
- Files written by the board are format 0, one track, 480 ticks per beat at 480000 µs per beat, so 1 tick = 1 ms. End of Track marks the loop length.
- The board reads format 0 (or single-track format 1) files with any PPQ and tempo changes, keeping only note on/off. The desktop converts other files to that form before uploading (`midi_files.board_bytes`).
- The desktop opens the port with DTR and RTS released so the dev board's auto-reset circuit does not reboot the ESP32 and wipe the take.
