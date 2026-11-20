"""Manage standard .mid files stored in desktop_app/recordings."""

from datetime import datetime
from pathlib import Path
import shutil

import mido

RECORDINGS = Path(__file__).resolve().parent / "recordings"


def ensure_folder():
    RECORDINGS.mkdir(parents=True, exist_ok=True)
    return RECORDINGS


def list_recordings():
    return sorted((p for p in ensure_folder().iterdir() if p.is_file() and p.suffix.lower() in (".mid", ".midi")), key=lambda p: p.name.lower())


def midi_info(path):
    midi = mido.MidiFile(path)
    notes = sum(
        1
        for track in midi.tracks
        for event in track
        if event.type == "note_on" and event.velocity > 0
    )
    try:
        duration = f"{midi.length:.1f} s"
    except (ValueError, TypeError):  # Type-2 MIDI has no single combined length.
        duration = "N/A"
    return len(midi.tracks), notes, duration


def import_midi(source):
    """Validate and copy a MIDI file into the recordings directory."""
    source = Path(source)
    mido.MidiFile(source)  # Raises if input is not a readable MIDI file.
    destination = ensure_folder() / source.name
    if source.resolve() == destination.resolve():
        return destination
    if destination.exists():
        raise FileExistsError(f"{destination.name} already exists.")
    shutil.copy2(source, destination)
    return destination


def create_demo_recording():
    """Generate a local MIDI sample; does NOT record anything from an ESP32."""
    midi = mido.MidiFile(ticks_per_beat=480)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.MetaMessage("track_name", name="Demo (computer-generated)", time=0))
    # Three sequential quarter-note pitches, each followed by a short rest.
    for note in (60, 64, 67):
        track.append(mido.Message("note_on", note=note, velocity=85, time=60))
        track.append(mido.Message("note_off", note=note, velocity=0, time=420))
    track.append(mido.MetaMessage("end_of_track", time=0))
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    output = ensure_folder() / f"demo_{stamp}.mid"
    midi.save(output)
    return output


def normalize_for_synth(data):
    """Flatten MIDI to one 120-BPM/480-PPQ track; keep real note timing.

    The ESP32 currently handles note events only, not MIDI instrument changes,
    sustain CC, pitch bends, or tempo changes. Raises on oversized conversion.
    """
    import io
    source = mido.MidiFile(file=io.BytesIO(data))
    if source.type == 2:
        raise ValueError("Type-2 MIDI has independent timelines; use a type-0/1 file.")
    merged = mido.merge_tracks(source.tracks)
    output = mido.MidiFile(type=0, ticks_per_beat=480)
    track = mido.MidiTrack()
    output.tracks.append(track)
    tempo = 500000
    absolute_seconds = 0.0
    last_tick = 0
    event_count = 0
    for message in merged:
        absolute_seconds += mido.tick2second(message.time, source.ticks_per_beat, tempo)
        if message.type == "set_tempo":
            tempo = message.tempo
        if message.type not in ("note_on", "note_off"):
            continue
        tick = round(absolute_seconds * 960)  # 480 ticks/0.5 seconds
        track.append(message.copy(time=tick-last_tick, channel=0))
        last_tick = tick
        event_count += 1
    if event_count == 0:
        raise ValueError("MIDI file has no notes.")
    if event_count > 2048:
        raise ValueError("MIDI file exceeds the ESP32's 2048-event capacity.")
    track.append(mido.MetaMessage("end_of_track", time=0))
    file = io.BytesIO()
    output.save(file=file)
    encoded = file.getvalue()
    if len(encoded) > 16384:
        raise ValueError("MIDI file exceeds the ESP32's 16 KiB limit.")
    return encoded
