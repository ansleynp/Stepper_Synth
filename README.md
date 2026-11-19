# Stepper Synth — combined ESP32 + desktop-app repository

One VS Code folder, one GitHub repository, two independent programs:

- ESP32 firmware: ESP-IDF written in C, built with PlatformIO (`platformio.ini`, `src/`).
- Desktop app: Python, Tkinter GUI (`desktop_app/`).

The desktop GUI works **without** an ESP32 attached. Its local MIDI file management is functional; **USB recording transfer is intentionally not yet implemented**. The ESP32 firmware is a boot/logging scaffold, not motor-control firmware.

## Prerequisites (Windows)

1. Install VS Code and Git.
2. Install Python 3 and make sure `py -3 --version` works in a terminal.
3. Install VS Code extensions **PlatformIO IDE** and **Python** (Microsoft).
4. Open this entire `Stepper-Synth-Starter` folder in VS Code using **File > Open Folder**.

## Run the Python desktop app (no board needed)

**WINDOWS** In the VS Code terminal from the project root:

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r desktop_app\requirements.txt
.\.venv\Scripts\python.exe desktop_app\main.py
```

**MAC(OS)** In the VS Code terminal from the project root:

```shell
./.venv/bin/python -m pip install -r desktop_app/requirements.txt
./.venv/bin/python desktop_app/main.py
```

If `py` isn't recognized, install Python from python.org or use `python` instead.

- **Create demo MIDI** makes a computer-generated `.mid` recording.
- **Import .mid file** copies a MIDI file into `desktop_app/recordings`.
- The recording list shows file info.
- **Refresh** lists available USB serial/COM ports, if any.
- **Open port** just opens a COM port. It does NOT verify an ESP32 recording protocol.
- **Save from Synth** and **Load onto Synth** show explanation dialogs until both programs support a transfer protocol.

VS Code: Command Palette > `Python: Select Interpreter` > select `.venv` to use this Python interpreter for Run/Debug.

## Build ESP32 firmware (board can be added later)

- Open **the repo root**, not `desktop_app` alone.
- VS Code PlatformIO toolbar or PlatformIO > Project Tasks > `esp32dev` > **Build**.
- Once the ESP32-WROOM-32 development board is connected via a USB *data* cable: **Upload**, then **Monitor** (115200 baud).
- Expected firmware log includes `Stepper Synth starter firmware booted.`
- Close the desktop app's serial port before uploading or opening PlatformIO Serial Monitor. Usually only one program can own a COM port at once.

The `esp32dev` board is a starting config for a common original ESP32-WROOM-32 dev board. Confirm the actual board and USB driver once hardware is available.

## Source control

Use VS Code Source Control > **Initialize Repository** if the folder isn't already a Git repository, commit the tracked files, then **Publish to GitHub**. If your team already has a Stepper Synth repository, add these files to that repo instead of publishing a second repository. Avoid overwriting existing firmware and `platformio.ini`.

The `.gitignore` deliberately excludes `.venv/`, PlatformIO build output, and recorded `.mid` files. Do not put a real recording into GitHub unless the team explicitly wants it tracked.

## Integration decisions still to make

Before actual upload/export implementation, agree on a **versioned serial protocol** with the embedded team:

- MIDI note-on/note-off event representation (pitch, velocity, time/timestamp, channel as needed)
- Timebase and data sizes; maximum recording length in ESP32 SRAM
- Serial framing, error detection, timeouts, acknowledgments
- Whether a newly loaded recording replaces the previous one
- Whether sustain/control-change data is supported and how it maps to the synth
- USB-to-UART circuit on final custom PCB (ESP32-WROOM-32 has no native USB)

Keep real-time motor STEP generation separate from slow desktop serial transfers.
