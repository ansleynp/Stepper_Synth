"""Diatonic harmony and three-voice motor assignments."""
from dataclasses import dataclass

LOW, HIGH = 45, 84
TONICS = "C Db D Eb E F F# G Ab A Bb B".split()
PITCHES = "C C♯ D E♭ E F F♯ G A♭ A B♭ B".split()
SCALES = {"Major": (0, 2, 4, 5, 7, 9, 11), "Natural minor": (0, 2, 3, 5, 7, 8, 10)}
FAMILIES = ("Triad", "Sixth", "Seventh")


def pitch(n):
    return f"{PITCHES[n % 12]}{n // 12 - 1}"


def scale_pitch(n, tonic, mode, octave=True):
    """Spell diatonic notes using each letter of the scale exactly once."""
    interval = (n - TONICS.index(tonic)) % 12
    scale = SCALES[mode]
    if interval not in scale:
        return pitch(n) if octave else PITCHES[n % 12]
    letters = "CDEFGAB"
    index = (letters.index(tonic[0]) + scale.index(interval)) % 7
    natural = (0, 2, 4, 5, 7, 9, 11)[index]
    alteration = (n - natural + 6) % 12 - 6
    accidental = "♯" * max(alteration, 0) + "♭" * max(-alteration, 0)
    number = str((n - alteration) // 12 - 1) if octave else ""
    return letters[index] + accidental + number


@dataclass(frozen=True)
class Chord:
    name: str
    notes: tuple
    roles: tuple
    degree: str
    detail: str


def chord(root, tonic, mode, family):
    scale = SCALES[mode]
    interval = (root - TONICS.index(tonic)) % 12
    if interval not in scale:
        return Chord(pitch(root), (root,), ("Root",), "Outside key", "Single-note pass-through")
    i = scale.index(interval)
    def steps(n):
        return (scale[(i + n) % 7] - scale[i]) % 12
    third, fifth = steps(2), steps(4)
    quality = "diminished" if fifth == 6 else "minor" if third == 3 else "major"
    roman = ("I", "II", "III", "IV", "V", "VI", "VII")[i]
    if third == 3:
        roman = roman.lower() + ("°" if fifth == 6 else "")
    offset, role = fifth, "Fifth"
    label, detail = quality, "Root · third · fifth"
    if family != "Triad" and fifth != 6:
        if family == "Sixth":
            offset, role = steps(5), "Sixth"
            label = quality + (" sixth" if offset == 9 else " (♭6)")
        else:
            offset, role = steps(6), "Seventh"
            label = {(4, 11): "major 7", (4, 10): "dominant 7",
                     (3, 10): "minor 7", (3, 11): "minor-major 7"}[third, offset]
        detail = "Fifth omitted · three-voice voicing"
    pairs = [(root, "Root")]
    for delta, name in [(third, "Third"), (offset, role)]:
        value = root + delta
        pairs.append((value if value <= HIGH else value - 12, name))
    pairs.sort()
    return Chord(f"{scale_pitch(root, tonic, mode, False)} {label}", tuple(n for n, _ in pairs),
                 tuple(r for _, r in pairs), roman, detail)


def identify(notes):
    pcs = {n % 12 for n in notes}
    shapes = {(0,4,7): "major", (0,3,7): "minor", (0,3,6): "diminished",
              (0,4,7,10): "dominant 7", (0,4,7,11): "major 7",
              (0,3,7,10): "minor 7", (0,4,7,9): "sixth", (0,3,7,9): "minor sixth"}
    names = [f"{PITCHES[r]} {shapes[s]}" for r in sorted(pcs)
             if (s := tuple(sorted((p-r) % 12 for p in pcs))) in shapes]
    return " / ".join(names) or (" · ".join(pitch(n) for n in sorted(notes)) or "Waiting for notes")


INTERVALS = ("unison", "minor 2nd", "major 2nd", "minor 3rd", "major 3rd", "perfect 4th",
             "tritone", "perfect 5th", "minor 6th", "major 6th", "minor 7th", "major 7th")


def interval_name(semitones):
    """Describe a transposition, e.g. +7 -> 'perfect 5th up', -12 -> '1 octave down'."""
    if semitones == 0:
        return "original key"
    octaves, rest = divmod(abs(semitones), 12)
    parts = ([f"{octaves} octave{'s' if octaves > 1 else ''}"] if octaves else []) + \
            ([INTERVALS[rest]] if rest else [])
    return " + ".join(parts) + (" up" if semitones > 0 else " down")
