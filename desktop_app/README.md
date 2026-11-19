# Stepper Synth Studio

Copy this desktop_app folder beside the firmware project's src folder. Keep your existing recordings directory when updating. The Python program runs on the computer.

## Run from the project root

With the project's existing virtual environment, use these commands on macOS.

```sh
./.venv/bin/python -m pip install -r desktop_app/requirements.txt
./.venv/bin/python desktop_app/main.py
```

On Windows PowerShell, use the following.

```powershell
.\.venv\Scripts\python.exe -m pip install -r desktop_app\requirements.txt
.\.venv\Scripts\python.exe desktop_app\main.py
```

For a fresh setup, install Python 3.13.x (3.13.1 or later) with Tk support. From the project root, run `python3.13 -m venv .venv` on Mac or `py -3.13 -m venv .venv` on Windows, then use the commands above.

## Try it offline

Choose Auto, C, Major and Triad. Click C4 on the on-screen keyboard. The three motor cards read C4, E4 and G4. Click D4 for D minor. Click C6 to see E5, G5 and C6 fit within the instrument range. Out-of-key roots pass through as single notes. Press and hold an on-screen piano key to hear its resolved notes together through the computer speakers. Release to stop. With Triad selected, Auto stacks the third and fifth of the selected scale over each in-key root. For C major the seven chords are C major, D minor, E minor, F major, G major, A minor and B diminished. Switch to D major and the same D4 becomes D major. Computer preview uses pygame-ce for audio. Board-reported notes use the physical motors.

Sixth and Seventh select deterministic in-key voicings. The app keeps the root and third and omits the fifth to fit three motors. Diminished triads keep the diminished fifth. Identify lets you click multiple keys to toggle a held set. Chord recognition lists supported matching names, including multiple interpretations, or shows the pitches. Preview motor slots in Identify use the lowest three held keys.

Recordings supports demo creation, validated import, track count, note count and duration. MIDI files live in desktop_app/recordings. Existing recordings are picked up at launch.

## Connect the instrument

Connect the board with a USB data cable and close any other serial monitor. Select the port and click Connect. The starter boot-message firmware cannot acknowledge studio requests. With compatible firmware, select settings and click Apply to synth. The ESP32 plays from the uploaded map on physical MIDI key events. The desktop shows the pitches reported by the board. While live, changes to the controls take effect on the next successful Apply. Exit live restores ordinary play. Synth recording transfers are available in ordinary mode when the board advertises that capability. See FIRMWARE_INTERFACE.md before integrating the desktop app with your firmware.

The current color choices are rule-based. A learned Harmony Color slider needs listener ratings and a trained model. This package contains the desktop app only.
