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
