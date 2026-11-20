"""Manage standard .mid files stored in desktop_app/recordings."""

from datetime import datetime
import io
from pathlib import Path
import shutil
import tempfile
from typing import NamedTuple

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


# The synth stores one take as note on/off events with millisecond times.
# 480 ticks per beat at 480000 microseconds per beat makes 1 tick = 1 ms.
BOARD_TICKS = 480
BOARD_TEMPO = 480_000


DRUM_CHANNEL = 9  # General MIDI channel 10: percussion, never transposed


def shift_note(note, semitones):
    """Transpose one MIDI note, folding by octaves to stay within 0-127."""
    shifted = note + semitones
    while shifted < 0:
        shifted += 12
    while shifted > 127:
        shifted -= 12
    return shifted


def transposable(msg):
    return msg.type in ("note_on", "note_off", "polytouch") and msg.channel != DRUM_CHANNEL


def transpose_copy(source, semitones):
    """Save a transposed copy of a MIDI file beside the recordings. All tracks,
    timing and other events are kept. Returns (path, folded_note_events)."""
    source = Path(source)
    midi = mido.MidiFile(source)
    folded = 0
    for track in midi.tracks:
        for i, msg in enumerate(track):
            if transposable(msg):
                note = shift_note(msg.note, semitones)
                folded += note != msg.note + semitones
                track[i] = msg.copy(note=note)
    folder = ensure_folder()
    stem = f"{source.stem}_{semitones:+d}st"
    destination, n = folder / f"{stem}.mid", 2
    while destination.exists():
        destination, n = folder / f"{stem}_{n}.mid", n + 1
    midi.save(destination)
    return destination, folded


class BoardTake(NamedTuple):
    data: bytes          # single-track MIDI file to upload
    events: int          # note on/off events uploaded
    seconds: float       # loop length uploaded
    folded: int          # note events folded by octaves to stay within 0-127
    truncated: bool      # True if the end was cut off to fit the synth
    full_events: int     # note on/off events in the whole file
    full_seconds: float  # length of the whole file


def board_bytes(source, transpose=0, max_events=None):
    """Convert any type 0/1 MIDI file into the single-track millisecond form the
    synth stores. Merges tracks, applies tempo changes, transposes by
    `transpose` semitones and keeps only note on/off. If the file has more
    than `max_events` note events it is cut short so it still fits: notes
    still sounding at the cut are ended there and the loop ends at the cut."""
    midi = mido.MidiFile(source)
    if midi.type == 2:
        raise ValueError("Type 2 MIDI files (independent patterns) are not supported.")
    events, now, folded = [], 0.0, 0
    for msg in midi:  # tracks merged; msg.time = seconds since the previous message
        now += msg.time
        if msg.type in ("note_on", "note_off"):
            on = msg.type == "note_on" and msg.velocity > 0
            note = msg.note
            if transpose and transposable(msg):
                note = shift_note(msg.note, transpose)
                folded += note != msg.note + transpose
            events.append((round(now * 1000), on, note, msg.velocity))
    if not any(on for _, on, _, _ in events):
        raise ValueError("This MIDI file has no notes to play.")
    full_events = len(events)
    full_length = max(round(now * 1000), events[-1][0] + 1)
    length = full_length

    if max_events and full_events > max_events:
        if max_events < 2:
            raise ValueError("The synth can't hold any notes.")
        kept, held, cut = [], set(), None
        for ms, on, note, velocity in events:
            after = held | {note} if on else held - {note}
            if len(kept) + 1 + len(after) > max_events:  # keep room to end held notes
                cut = ms
                break
            kept.append((ms, on, note, velocity))
            held = after
        kept += [(cut, False, note, 64) for note in sorted(held)]
        events = kept
        length = max(cut, events[-1][0] + 1)
        if not any(on for _, on, _, _ in events):
            raise ValueError("No notes fit on the synth.")

    out = mido.MidiFile(type=0, ticks_per_beat=BOARD_TICKS)
    track = mido.MidiTrack()
    out.tracks.append(track)
    track.append(mido.MetaMessage("set_tempo", tempo=BOARD_TEMPO, time=0))
    last = 0
    for ms, on, note, velocity in events:
        kind = "note_on" if on else "note_off"
        track.append(mido.Message(kind, note=note, velocity=velocity, time=ms - last))
        last = ms
    track.append(mido.MetaMessage("end_of_track", time=length - last))
    buffer = io.BytesIO()
    out.save(file=buffer)
    return BoardTake(buffer.getvalue(), len(events), length / 1000, folded,
                     len(events) < full_events, full_events, full_length / 1000)


def remove_recording(name):
    """Delete one file from the recordings folder (and nothing outside it)."""
    folder = ensure_folder().resolve()
    path = (folder / name).resolve()
    if path.parent != folder or not path.is_file():
        raise FileNotFoundError(f"{name} is not in the recordings folder.")
    path.unlink()


BAD_NAME_CHARS = set('/\\:*?"<>|')


def rename_recording(old, new):
    """Rename a file in the recordings folder. Adds .mid if the new name has no
    MIDI extension. Returns the new path."""
    folder = ensure_folder().resolve()
    source = (folder / old).resolve()
    if source.parent != folder or not source.is_file():
        raise FileNotFoundError(f"{old} is not in the recordings folder.")
    new = new.strip()
    if not new or new.startswith(".") or any(c in BAD_NAME_CHARS or ord(c) < 32 for c in new):
        raise ValueError("Use a name without / \\ : * ? \" < > | that doesn't start with a dot.")
    if Path(new).suffix.lower() not in (".mid", ".midi"):
        new += ".mid"
    if len(new.encode()) > 255:
        raise ValueError("That name is too long.")
    destination = folder / new
    if destination.exists() and not destination.samefile(source):  # case-only renames are fine
        raise FileExistsError(f"{new} already exists.")
    source.rename(destination)
    return destination


def default_take_name():
    return f"synth_{datetime.now().strftime('%Y%m%d_%H%M%S')}.mid"


def save_bytes(data, destination):
    """Validate MIDI bytes and write them atomically to destination."""
    mido.MidiFile(file=io.BytesIO(data))  # Raises if the board sent something unreadable.
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as out:
        temp = Path(out.name)
    try:
        temp.write_bytes(data)
        temp.replace(destination)
    finally:
        temp.unlink(missing_ok=True)
    return destination
